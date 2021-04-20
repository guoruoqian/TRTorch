#include "NvInfer.h"
#include "core/conversion/converters/converters.h"
#include "core/conversion/tensorcontainer/TensorContainer.h"
#include "core/util/prelude.h"
#include "torch/torch.h"

#include <ATen/ATen.h>
#include <vector>

namespace trtorch {
namespace core {
namespace conversion {
namespace converters {
namespace impl {
namespace {

auto unsqueeze_registrations TRTORCH_UNUSED = RegisterNodeConversionPatterns().pattern(
    {"aten::unsqueeze(Tensor(a) self, int dim) -> (Tensor(a))",
     [](ConversionCtx* ctx, const torch::jit::Node* n, args& args) -> bool {
       auto self = args[0].ITensorOrFreeze(ctx);
       auto dim = args[1].unwrapToInt();

       auto selfDim = util::toVec(self->getDimensions());
       auto nbDims = self->getDimensions().nbDims;
       TRTORCH_ASSERT(
           dim <= nbDims && dim >= -(nbDims + 1),
           "imension out of range (expected to be in range of [" << -(nbDims + 1) << ", " << nbDims << "], but got "
                                                                 << dim << ")");
       if (dim < 0) {
         dim = nbDims + 1 + dim;
       }

       auto shuffle_layer = ctx->net->addShuffle(*self);
       TRTORCH_CHECK(shuffle_layer, "Unable to create shuffle layer from node: " << *n);
       shuffle_layer->setReshapeDimensions(util::unsqueezeDims(self->getDimensions(), dim));

       auto out = ctx->AssociateValueAndTensor(n->outputs()[0], shuffle_layer->getOutput(0));

       LOG_DEBUG("Output tensor shape: " << out->getDimensions());

       return true;
     }});

} // namespace
} // namespace impl
} // namespace converters
} // namespace conversion
} // namespace core
} // namespace trtorch