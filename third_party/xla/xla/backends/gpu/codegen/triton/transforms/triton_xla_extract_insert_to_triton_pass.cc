/* Copyright 2025 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <stdbool.h>

#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "xla/backends/gpu/codegen/triton/emitter_helpers.h"
#include "xla/backends/gpu/codegen/triton/ir/triton_xla_ops.h"
#include "xla/backends/gpu/codegen/triton/transforms/passes.h"
#include "xla/codegen/emitter_loc_op_builder.h"
#include "xla/stream_executor/device_description.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Types.h"

namespace mlir::triton::xla {

namespace xgt = ::xla::gpu::triton;

namespace {

#define GEN_PASS_DEF_TRITONXLAEXTRACTINSERTTOTRITONPASS
#include "xla/backends/gpu/codegen/triton/transforms/passes.h.inc"

template <typename T>
SmallVector<Value> GetValueRange(::xla::EmitterLocOpBuilder& builder,
                                 llvm::ArrayRef<T> array_ref) {
  SmallVector<mlir::Value> values;
  for (T value : array_ref) {
    values.push_back(builder.create<arith::ConstantIntOp>(
        value, builder.getIntegerType(sizeof(T) * 8)));
  }
  return values;
}

PointerType GetTensorPtrType(::xla::EmitterLocOpBuilder& builder, Type type) {
  return PointerType::get(xgt::StorageType(builder, type),
                          mlir::NVVM::kGlobalMemorySpace);
}

bool AreRankedTensors(ArrayRef<Type> types) {
  return llvm::all_of(types, [](mlir::Type type) {
    return mlir::isa<mlir::RankedTensorType>(type);
  });
}

void ComputeBoundaryChecks(std::vector<int32_t>& boundary_checks,
                           const TiledTensorType& tiled_tensor_type) {
  for (auto [dim_idx, sizes] :
       llvm::enumerate(llvm::zip(tiled_tensor_type.getOriginalShape(),
                                 tiled_tensor_type.getTileShape()))) {
    auto [dim_size, tile_size] = sizes;
    if (dim_size % tile_size) {
      boundary_checks.push_back(dim_idx);
    }
  }
}

struct RewriteFuncOp : mlir::OpRewritePattern<func::FuncOp> {
  using OpRewritePattern::OpRewritePattern;

  // Rewrite tensors<> to !tt.ptr<tensor>
  // Remove any returns. i.e. tt.return with no operands.
  mlir::LogicalResult matchAndRewrite(
      func::FuncOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);

    auto input_types = op.getFunctionType().getInputs();
    auto output_types = op.getFunctionType().getResults();

    if (!AreRankedTensors(input_types) || !AreRankedTensors(output_types)) {
      return rewriter.notifyMatchFailure(
          op, "Expected all inputs and results to have tensor type.");
    }

    SmallVector<Type> new_operand_types(input_types);
    for (auto&& [index, operand_type] : llvm::enumerate(new_operand_types)) {
      mlir::BlockArgument func_arg = op.getArgument(index);

      // !tt.ptr<> -> tensor
      auto cast_to_orig_type = builder.create<mlir::UnrealizedConversionCastOp>(
          operand_type, func_arg);
      func_arg.replaceAllUsesExcept(cast_to_orig_type.getResult(0),
                                    cast_to_orig_type);
      operand_type = GetTensorPtrType(
          builder, mlir::cast<TensorType>(operand_type).getElementType());
    }

    // Replace the function arguments with the new types.
    mlir::Block* entry_block = &op.getBody().front();
    for (auto [arg, arg_type] :
         llvm::zip(entry_block->getArguments(), new_operand_types)) {
      arg.setType(arg_type);
    }

    auto new_function_type = FunctionType::get(
        op.getContext(), new_operand_types, /*result_types=*/{});
    auto new_func = rewriter.create<triton::FuncOp>(op.getLoc(), op.getName(),
                                                    new_function_type);

    rewriter.inlineRegionBefore(op.getRegion(), new_func.getFunctionBody(),
                                new_func.end());
    rewriter.replaceOp(op, new_func);

    auto terminator = new_func.getBody().front().getTerminator();
    rewriter.setInsertionPoint(terminator);
    rewriter.create<triton::ReturnOp>(new_func.getLoc());
    rewriter.eraseOp(terminator);

    return mlir::success();
  }
};

struct RewriteTile : mlir::OpRewritePattern<TileOp> {
  using OpRewritePattern::OpRewritePattern;

  // Rewriting TileOp as tt.make_tensor_ptr.
  mlir::LogicalResult matchAndRewrite(
      TileOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);

    // Order is 0, 1, ..., rank - 1.
    std::vector<int32_t> dim_order(op.getSizes().size());
    std::iota(dim_order.begin(), dim_order.end(), 0);

    // tensor -> !tt.ptr<>
    auto cast_to_tensor_ptr_type =
        builder
            .create<mlir::UnrealizedConversionCastOp>(
                GetTensorPtrType(builder,
                                 op.getTensor().getType().getElementType()),
                op.getTensor())
            .getResult(0);

    auto tensor_ptr =
        builder
            .create<MakeTensorPtrOp>(
                cast_to_tensor_ptr_type,
                GetValueRange(builder, op.getTensor().getType().getShape()),
                GetValueRange(builder, op.getStrides()),
                GetValueRange(builder, op.getOffsets()), op.getSizes(),
                dim_order)
            .getResult();

    // !tt.ptr<tensor> -> tiled_tensor
    auto cast_to_tiled_tensor_type =
        builder.create<mlir::UnrealizedConversionCastOp>(
            op.getTiledTensor().getType(), tensor_ptr);

    rewriter.replaceOp(op, cast_to_tiled_tensor_type);
    return mlir::success();
  }
};

struct RewriteExtract : mlir::OpRewritePattern<ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  // Rewriting ExtractOp as tt.advance + tt.load.
  mlir::LogicalResult matchAndRewrite(
      ExtractOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);

    // tiled_tensor -> !tt.ptr<tensor>
    auto cast_to_tensor_ptr_type =
        builder
            .create<mlir::UnrealizedConversionCastOp>(
                GetTensorPtrType(builder,
                                 RankedTensorType::get(
                                     op.getSrc().getType().getTileShape(),
                                     op.getSrc().getType().getElementType())),
                op.getSrc())
            .getResult(0);

    auto advance =
        builder.create<AdvanceOp>(cast_to_tensor_ptr_type.getType(),
                                  cast_to_tensor_ptr_type, op.getOffsets());
    std::vector<int32_t> boundary_checks;
    ComputeBoundaryChecks(boundary_checks, op.getSrc().getType());
    std::optional<PaddingOption> padding;
    if (!boundary_checks.empty()) {
      padding = PaddingOption::PAD_ZERO;
    }
    auto load = builder
                    .create<LoadOp>(advance, boundary_checks, padding,
                                    CacheModifier::NONE, EvictionPolicy::NORMAL,
                                    /*isVolatile=*/false)
                    .getResult();

    rewriter.replaceOp(op, load);
    return mlir::success();
  }
};

struct RewriteInsert : mlir::OpRewritePattern<InsertOp> {
  using OpRewritePattern::OpRewritePattern;

  // Rewriting InsertOp as tt.advance + tt.store.
  mlir::LogicalResult matchAndRewrite(
      InsertOp op, mlir::PatternRewriter& rewriter) const override {
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);

    // tiled_tensor -> !tt.ptr<tensor>
    auto cast_dst_to_tensor_ptr_type =
        builder
            .create<mlir::UnrealizedConversionCastOp>(
                GetTensorPtrType(builder,
                                 RankedTensorType::get(
                                     op.getDst().getType().getTileShape(),
                                     op.getDst().getType().getElementType())),
                op.getDst())
            .getResult(0);

    auto advance =
        builder.create<AdvanceOp>(cast_dst_to_tensor_ptr_type.getType(),
                                  cast_dst_to_tensor_ptr_type, op.getOffsets());
    std::vector<int32_t> boundary_checks;
    ComputeBoundaryChecks(boundary_checks, op.getDst().getType());
    std::optional<PaddingOption> padding;
    if (!boundary_checks.empty()) {
      padding = PaddingOption::PAD_ZERO;
    }
    rewriter.create<StoreOp>(op->getLoc(), advance, op.getSrc(),
                             boundary_checks, CacheModifier::NONE,
                             EvictionPolicy::NORMAL);

    // InsertOp has a result, so we propagate it to the users.
    op->replaceAllUsesWith(ValueRange(op.getDst()));

    return mlir::success();
  }
};

// Rewriting tensor::InsertOp as tt.store.
struct RewriteScalarInsert : mlir::OpRewritePattern<tensor::InsertOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      tensor::InsertOp op, mlir::PatternRewriter& rewriter) const override {
    if (op.getDest().getType().getRank() != 0) {
      return rewriter.notifyMatchFailure(op, "Expected dest to be scalar.");
    }
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto ptr_type = GetTensorPtrType(builder, op.getScalar().getType());
    auto cast_dst_to_tensor_ptr_type =
        builder.create<mlir::UnrealizedConversionCastOp>(ptr_type, op.getDest())
            .getResult(0);
    builder.create<StoreOp>(cast_dst_to_tensor_ptr_type, op.getScalar(),
                            /*boundary_checks=*/std::vector<int32_t>{},
                            CacheModifier::NONE, EvictionPolicy::NORMAL);
    rewriter.replaceOp(op, op.getDest());
    return mlir::success();
  }
};

struct RewriteScalarExtract : mlir::OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern::OpRewritePattern;

  // Rewriting ExtractOp as tt.advance + tt.store.
  mlir::LogicalResult matchAndRewrite(
      tensor::ExtractOp op, mlir::PatternRewriter& rewriter) const override {
    if (op.getTensor().getType().getRank() != 0) {
      return rewriter.notifyMatchFailure(op, "Expected src to be scalar.");
    }
    ::xla::EmitterLocOpBuilder builder(op.getLoc(), rewriter);
    auto ptr_type = GetTensorPtrType(builder, op.getType());
    auto cast_src_to_tensor_ptr_type =
        builder
            .create<mlir::UnrealizedConversionCastOp>(ptr_type, op.getTensor())
            .getResult(0);
    auto scalar =
        builder.create<LoadOp>(cast_src_to_tensor_ptr_type, CacheModifier::NONE,
                               EvictionPolicy::NORMAL, /*isVolatile=*/false);
    rewriter.replaceOp(op, scalar.getResult());
    return mlir::success();
  }
};

struct TritonXLAExtractInsertToTritonPass
    : public impl::TritonXLAExtractInsertToTritonPassBase<
          TritonXLAExtractInsertToTritonPass> {
  explicit TritonXLAExtractInsertToTritonPass(
      const TritonXLAExtractInsertToTritonPassOptions& options)
      : TritonXLAExtractInsertToTritonPassBase(options) {}

  explicit TritonXLAExtractInsertToTritonPass(
      const ::xla::se::DeviceDescription& device_description, bool tma_enabled)
      : device_description(device_description), tma_enabled(tma_enabled) {}

  void runOnOperation() override {
    if (!gpu_device_info_.empty()) {
      ::xla::se::GpuDeviceInfoProto device_info;
      CHECK(tsl::protobuf::TextFormat::ParseFromString(gpu_device_info_,
                                                       &device_info));
      device_description = ::xla::se::DeviceDescription(device_info);
    }
    mlir::MLIRContext* mlir_context = &getContext();
    mlir::RewritePatternSet patterns(mlir_context);
    // clang-format off
    patterns.add<
        RewriteExtract,
        RewriteFuncOp,
        RewriteInsert,
        RewriteScalarExtract,
        RewriteScalarInsert,
        RewriteTile
    >( mlir_context);
    // clang-format on
    if (mlir::failed(
            mlir::applyPatternsGreedily(getOperation(), std::move(patterns)))) {
      signalPassFailure();
    }
  }

  ::xla::se::DeviceDescription device_description;
  bool tma_enabled;
};

}  // namespace

std::unique_ptr<mlir::Pass> CreateTritonXLAExtractInsertToTritonPass(
    const std::string& gpu_device_info, bool tma_enabled) {
  TritonXLAExtractInsertToTritonPassOptions options;
  options.gpu_device_info_ = gpu_device_info;
  options.tma_enabled_ = tma_enabled;
  return std::make_unique<TritonXLAExtractInsertToTritonPass>(options);
}

std::unique_ptr<mlir::Pass> CreateTritonXLAExtractInsertToTritonPass(
    const ::xla::se::DeviceDescription& device_description, bool tma_enabled) {
  return std::make_unique<TritonXLAExtractInsertToTritonPass>(
      device_description, tma_enabled);
}

}  // namespace mlir::triton::xla
