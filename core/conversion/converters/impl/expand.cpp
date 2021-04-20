#include "NvInfer.h"
#include "core/conversion/converters/converters.h"
#include "core/conversion/tensorcontainer/TensorContainer.h"
#include "core/util/prelude.h"
#include "core/util/trt_util.h"
#include "torch/torch.h"

#include <ATen/ATen.h>
#include <vector>

namespace trtorch {
namespace core {
namespace conversion {
namespace converters {
namespace impl {
namespace {

void addSliceInput(nvinfer1::Dims& dims, int idx, ConversionCtx* ctx, nvinfer1::ISliceLayer* slice) {
  int64_t rank = dims.nbDims;
  int32_t* tmp = new int32_t[rank];
  for (int i = 0; i < rank; i++)
    tmp[i] = dims.d[i];
  const nvinfer1::Dims d{1, {rank}, {}};
  const nvinfer1::Weights w{nvinfer1::DataType::kINT32, tmp, rank};
  auto t = ctx->net->addConstant(d, w)->getOutput(0);
  slice->setInput(idx, *t);
}

nvinfer1::ITensor* vec2Tensor(int32_t* dim, int count, ConversionCtx* ctx) {
  int64_t rank = count;
  const nvinfer1::Dims d{1, {rank}, {}};
  const nvinfer1::Weights w{nvinfer1::DataType::kINT32, dim, rank};
  return ctx->net->addConstant(d, w)->getOutput(0);
}

nvinfer1::ITensor* concat(int max_rank, int old_rank, ConversionCtx* ctx, nvinfer1::ITensor* tensor) {
  if (max_rank - old_rank > 0) {
    int32_t* tmp = new int32_t[max_rank - old_rank];
    for (int i = 0; i < (max_rank - old_rank); i++)
      tmp[i] = 1;
    auto max_rank_tensor = vec2Tensor(tmp, max_rank - old_rank, ctx);
    auto in_shape_tensor = ctx->net->addShape(*tensor)->getOutput(0);
    nvinfer1::ITensor* const args[2] = {max_rank_tensor, in_shape_tensor};
    return ctx->net->addConcatenation(args, 2)->getOutput(0);
  } else if (max_rank - old_rank == 0) {
    return ctx->net->addShape(*tensor)->getOutput(0);
  }
}

bool add_expand(ConversionCtx* ctx, const torch::jit::Node* n, nvinfer1::ITensor* in, nvinfer1::Dims expandedDims) {
  auto input_dims = in->getDimensions();
  TRTORCH_CHECK(
      input_dims.nbDims <= expandedDims.nbDims,
      "Number of dimensions of the desired expansion must be greater than or equal to the number of input dimensions");

  // Validate the expansion. Eg: an input of [3, 1] can be expanded to [1, 3, 4] but not [3, 4, 1]
  for (int64_t i = expandedDims.nbDims - 1; i >= 0; --i) {
    int64_t offset = expandedDims.nbDims - 1 - i;
    int64_t dim = input_dims.nbDims - 1 - offset;
    int64_t size = (dim >= 0) ? input_dims.d[dim] : 1;
    int64_t targetSize = expandedDims.d[i];
    if (size != targetSize) {
      if (size != 1) {
        TRTORCH_THROW_ERROR(
            "The expanded size of tensor (" << targetSize << ")"
                                            << " must match the existing size (" << size << ")"
                                            << " at dimension " << i);
      }
    }
  }

  auto num_expand_dims = expandedDims.nbDims - input_dims.nbDims;
  if (num_expand_dims > 0) {
    nvinfer1::Dims reshape_dims;
    reshape_dims.nbDims = expandedDims.nbDims;
    for (int64_t i = 0; i < num_expand_dims; i++) {
      reshape_dims.d[i] = 1;
    }
    for (int64_t i = 0; i < input_dims.nbDims; i++) {
      reshape_dims.d[num_expand_dims + i] = input_dims.d[i];
    }
    // Add a reshape layer to expand dims
    auto reshape_layer = ctx->net->addShuffle(*in);
    reshape_layer->setReshapeDimensions(reshape_dims);
    in = reshape_layer->getOutput(0);
    LOG_DEBUG("Input reshaped to : " << in->getDimensions() << " from " << input_dims);
  }

  // Start the slicing from beginning of tensor since this is an expand layer
  std::vector<int64_t> start_vec(expandedDims.nbDims, 0);
  auto start_offset = util::toDims(c10::IntArrayRef(start_vec));

  // Set the stride of non singleton dimension to 1
  std::vector<int64_t> strides_vec(expandedDims.nbDims, 0);
  for (int64_t i = 0; i < expandedDims.nbDims; i++) {
    strides_vec[i] = (in->getDimensions().d[i] != 1);
  }

  auto strides = util::toDims(c10::IntArrayRef(strides_vec));
  // Slice layer does the expansion in TRT. Desired output size is specified by expandedDims
  auto slice_layer = ctx->net->addSlice(*in, start_offset, expandedDims, strides);
  slice_layer->setName(util::node_info(n).c_str());

  auto out = ctx->AssociateValueAndTensor(n->outputs()[0], slice_layer->getOutput(0));

  LOG_DEBUG("Expand layer output tensor shape: " << out->getDimensions());

  return true;
}

bool add_expand_dynamic(
    ConversionCtx* ctx,
    const torch::jit::Node* n,
    nvinfer1::ITensor* in,
    nvinfer1::ITensor* expandedDimsTensor) {
  auto input_rank = in->getDimensions().nbDims;
  auto output_rank = expandedDimsTensor->getDimensions().d[0];
  TRTORCH_CHECK(
      input_rank <= output_rank,
      "Number of dimensions of the desired expansion must be greater than or equal to the number of input dimensions");

  size_t max_rank = std::max(input_rank, output_rank);

  // Dimensions are right alignment
  auto new_input_shape_tensor = concat(max_rank, input_rank, ctx, in);
  auto new_output_shape_tensor = expandedDimsTensor;

  // Add a reshape layer to expand dims
  auto shuffle = ctx->net->addShuffle(*in);
  shuffle->setInput(1, *new_input_shape_tensor);

  // Start the slicing from beginning of tensor since this is an expand layer
  std::vector<int64_t> start_vec(max_rank, 0);
  nvinfer1::Dims starts_dim = util::toDims(c10::IntArrayRef(start_vec));

  // broadcast
  // max(x,y) works unless x or y is 0.
  // min(x,y,1) yields 0 if x or y is 0, and 1 otherwise.
  // So compute max(x,y)*min(x,y,1).
  int32_t* one_tmp = new int32_t[max_rank];
  for (int i = 0; i < max_rank; i++)
    one_tmp[i] = 1;
  auto one = vec2Tensor(one_tmp, max_rank, ctx);
  auto min_y_1 =
      ctx->net->addElementWise(*new_output_shape_tensor, *one, nvinfer1::ElementWiseOperation::kMIN)->getOutput(0);
  auto min_x_y_1 =
      ctx->net->addElementWise(*new_input_shape_tensor, *min_y_1, nvinfer1::ElementWiseOperation::kMIN)->getOutput(0);
  auto max_x_y =
      ctx->net->addElementWise(*new_input_shape_tensor, *new_output_shape_tensor, nvinfer1::ElementWiseOperation::kMAX)
          ->getOutput(0);
  auto sizes = ctx->net->addElementWise(*max_x_y, *min_x_y_1, nvinfer1::ElementWiseOperation::kPROD)->getOutput(0);
  nvinfer1::Dims sizes_dim{-1, {}, {}};
  sizes_dim.nbDims = max_rank;

  // Compute (x > 1 ? 1 : 0) for x in newDims, assuming positive x, using only TensorRT operations.
  // min(1, sub(input_shape, 1))
  int32_t* one_vector_tmp = new int32_t[1];
  one_vector_tmp[0] = 1;
  auto one_vector = vec2Tensor(one_vector_tmp, 1, ctx);
  auto x_sub_one = ctx->net->addElementWise(*new_input_shape_tensor, *one_vector, nvinfer1::ElementWiseOperation::kSUB)
                       ->getOutput(0);
  auto strides = ctx->net->addElementWise(*one_vector, *x_sub_one, nvinfer1::ElementWiseOperation::kMIN)->getOutput(0);
  nvinfer1::Dims strides_dim{-1, {}, {}};
  strides_dim.nbDims = max_rank;

  // Slice layer does the expansion in TRT. Desired output size is specified by expandedDimsTensor
  auto slice = ctx->net->addSlice(*shuffle->getOutput(0), starts_dim, sizes_dim, strides_dim);
  addSliceInput(starts_dim, 1, ctx, slice);
  slice->setInput(2, *sizes);
  slice->setInput(3, *strides);

  auto out_tensor = ctx->AssociateValueAndTensor(n->outputs()[0], slice->getOutput(0));

  LOG_DEBUG("Expand layer output tensor shape: " << out_tensor->getDimensions());

  return true;
}

auto expand_registrations TRTORCH_UNUSED =
    RegisterNodeConversionPatterns()
        .pattern({"aten::expand(Tensor(a) self, int[] size, *, bool implicit=False) -> (Tensor(a))",
                  [](ConversionCtx* ctx, const torch::jit::Node* n, args& args) -> bool {
                    auto in = args[0].ITensor();
                    auto input_dims = in->getDimensions();
                    auto expanded_size = args[1].unwrapToIntList();
                    auto expandedDims = util::toDims(expanded_size);
                    LOG_DEBUG("(expand layer) Expand input from " << input_dims << " to " << expandedDims);
                    if (ctx->input_is_dynamic) {
                      int32_t* tmp = new int32_t[expanded_size.size()];
                      for (int i = 0; i < expanded_size.size(); i++)
                        tmp[i] = expanded_size[i];
                      auto expandedDimsTensor = vec2Tensor(tmp, expanded_size.size(), ctx);
                      return add_expand_dynamic(ctx, n, in, expandedDimsTensor);
                    } else {
                      return add_expand(ctx, n, in, expandedDims);
                    }
                  }})
        .pattern({"aten::expand_as(Tensor(a) self, Tensor other) -> (Tensor(a))",
                  [](ConversionCtx* ctx, const torch::jit::Node* n, args& args) -> bool {
                    // TODO: Currently expand supports static shapes. Need to explore if the same code can be extended
                    // to dynamic expansion.
                    auto in = args[0].ITensor();
                    auto input_dims = in->getDimensions();
                    auto targetTensor = args[1].ITensor();
                    auto targetDims = targetTensor->getDimensions();
                    LOG_DEBUG("(expand_as layer) Expand input from " << input_dims << " to " << targetDims);
                    if (ctx->input_is_dynamic) {
                      return add_expand_dynamic(ctx, n, in, ctx->net->addShape(*targetTensor)->getOutput(0));
                    } else {
                      return add_expand(ctx, n, in, targetDims);
                    }
                  }})
        .pattern({"aten::repeat(Tensor self, int[] repeats) -> (Tensor)",
                  [](ConversionCtx* ctx, const torch::jit::Node* n, args& args) -> bool {
                    auto in = args[0].ITensorOrFreeze(ctx);
                    auto input_dims = in->getDimensions();
                    auto repeats = args[1].unwrapToIntList().vec();
                    TRTORCH_CHECK(
                        static_cast<int64_t>(repeats.size()) >= input_dims.nbDims,
                        "Number of repeat dimensions cannot be smaller than number of input dimensions");
                    auto num_expand_dims = repeats.size() - input_dims.nbDims;

                    if (ctx->input_is_dynamic) {
                      int input_rank = input_dims.nbDims;
                      int output_rank = repeats.size();
                      auto new_input_shape_tensor = concat(output_rank, input_rank, ctx, in);

                      // Add a reshape layer to expand dims
                      auto shuffle = ctx->net->addShuffle(*in);
                      shuffle->setInput(1, *new_input_shape_tensor);
                      in = shuffle->getOutput(0);
                    } else {
                      if (num_expand_dims > 0) {
                        nvinfer1::Dims reshape_dims;
                        reshape_dims.nbDims = repeats.size();
                        for (int i = 0; i < num_expand_dims; i++) {
                          reshape_dims.d[i] = 1;
                        }
                        for (int i = 0; i < input_dims.nbDims; i++) {
                          reshape_dims.d[num_expand_dims + i] = input_dims.d[i];
                        }
                        // Add a reshape layer to expand dims
                        auto reshape_layer = ctx->net->addShuffle(*in);
                        reshape_layer->setReshapeDimensions(reshape_dims);
                        in = reshape_layer->getOutput(0);
                        LOG_DEBUG("Input reshaped to : " << in->getDimensions() << " from " << input_dims);
                      }
                      LOG_DEBUG("Repeats: " << repeats);
                    }

                    // Concat across all repeat axes.
                    // TODO: Implementation might not be performant. Explore other strategies to improve performance.
                    for (int64_t i = repeats.size() - 1; i >= 0; --i) {
                      std::vector<nvinfer1::ITensor*> tensors_vec;
                      for (int64_t j = 0; j < repeats[i]; j++) {
                        tensors_vec.push_back(in);
                      }
                      auto concat_layer = ctx->net->addConcatenation(tensors_vec.data(), tensors_vec.size());
                      concat_layer->setAxis(i);
                      in = concat_layer->getOutput(0);
                    }

                    auto out = ctx->AssociateValueAndTensor(n->outputs()[0], in);

                    LOG_DEBUG("Repeat layer output tensor shape: " << in->getDimensions());
                    return true;
                  }});

} // namespace
} // namespace impl
} // namespace converters
} // namespace conversion
} // namespace core
} // namespace trtorch
