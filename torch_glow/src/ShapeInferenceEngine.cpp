// Copyright 2004-present Facebook. All Rights Reserved.

#include <ATen/WrapDimUtils.h>
#include <iostream>
#include <string>
#include <torch/script.h>
#include <unordered_set>
#include <vector>

#include "ShapeInferenceEngine.h"

#include "glow/Support/Error.h"
#include "glow/Support/Support.h"

namespace glow {

ShapeInferenceEngine::ShapeInferenceEngine(
    const torch::jit::Graph &graph, const at::ArrayRef<at::IValue> &inputs,
    const std::string &fusionNodeSymbol)
    : graph_(graph), inputs_(inputs), fusionNodeSymbol_(fusionNodeSymbol){};

void ShapeInferenceEngine::getNodeInputShape(const torch::jit::Node *node,
                                             MetaStack &inputMetas) {
  for (auto input : node->inputs()) {
    auto it = shapeMap_.find(input);
    CHECK(it != shapeMap_.end());
    inputMetas.emplace_back(shapeMap_[input]);
  }
}

const MetaStack &ShapeInferenceEngine::getGraphOutputShape() {
  return outputShape_;
}

const std::unordered_map<const torch::jit::Value *, VariableMeta> &
ShapeInferenceEngine::getVariableMap() {
  return shapeMap_;
}

Error ShapeInferenceEngine::shapeOnNode(const torch::jit::Node *node) {

  /// Get op symbol
  const auto kind = node->kind();
  const std::string symbol = kind.toQualString();
  /// Extract shapes of inputs from shape mapping
  MetaStack inputMetas;

  /// The output of each Op shape function could be either the shape or int
  /// value generated by prim::consant or prim::ListContruct.
  /// The \p outputShapesOrValues is to store outputs of ops shape function.
  std::vector<TensorShape> outputShapesOrValues(1);

  getNodeInputShape(node, inputMetas);

  // Get output shape or int value of the ops without actual computation
  if (symbol == "glow::fused_stack") {
    int64_t dim = node->i(at::attr::dim);
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               fusedStack(inputMetas, dim));
  } else if (symbol == "fb::embedding_bag_byte_rowwise_offsets" ||
             symbol == "quantized::embedding_bag_byte_rowwise_offsets") {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               embeddingBagByteRowwiseOffsets(inputMetas));
  } else if (symbol == "quantized::embedding_bag_4bit_rowwise_offsets") {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               embeddingBag4BitRowwiseOffsets(inputMetas));
  } else {
    switch (kind) {
    case c10::prim::Constant: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], primConstant(node));
      break;
    }
    case c10::aten::tanh:
    case c10::aten::relu:
    case c10::aten::sigmoid: {
      RETURN_ERR_IF_NOT(inputMetas.size() == 1,
                        "Expected 1 input shape for operators.");
      outputShapesOrValues[0] = inputMetas[0].shape<TensorShape>();
      break;
    }
    case c10::aten::sub:
    case c10::aten::pow:
    case c10::aten::mul:
    case c10::aten::add: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], binaryOp(inputMetas));
      break;
    }
    case c10::aten::mm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], mm(inputMetas));
      break;
    }
    case c10::aten::addmm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], addmm(inputMetas));
      break;
    }
    case c10::aten::bmm: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], bmm(inputMetas));
      break;
    }
    case c10::aten::t: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], t(inputMetas));
      break;
    }
    case c10::aten::transpose: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 transpose(inputMetas));
      break;
    }
    case c10::aten::flatten: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], flatten(inputMetas));
      break;
    }
    case c10::prim::FusedConcat: {
      int64_t dim = node->i(at::attr::dim);
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 fusedConcat(inputMetas, dim));
      break;
    }
    case c10::prim::ConstantChunk: {
      int64_t chunks = node->i(at::attr::chunks);
      int64_t dim = node->i(at::attr::dim);
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues,
                                 constantChunk(inputMetas, chunks, dim));
      break;
    }
    case c10::aten::chunk: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues, chunk(inputMetas));
      break;
    }
    case c10::prim::ListConstruct: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues,
                                 listConstruct(inputMetas));
      break;
    }
    case c10::aten::slice: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], slice(inputMetas));
      break;
    }
    case c10::aten::reshape: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], reshape(inputMetas));
      break;
    }
    case c10::aten::cat: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], cat(inputMetas));
      break;
    }
    case c10::aten::permute: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], permute(inputMetas));
      break;
    }
    case c10::aten::embedding_bag: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                                 embeddingBag(inputMetas));
      break;
    }
    case c10::aten::stack: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], stack(inputMetas));
      break;
    }
    case c10::prim::ListUnpack: {
      ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues, listUnpack(inputMetas));
      break;
    }
    default: {
      return MAKE_ERR(strFormat("Node's operator %s is not supported",
                                kind.toQualString()));
    }
    }
  }

  /// Put output into map
  /// For \p prim::Constant, the output could be either Tensor or NumberType.
  /// If the output is TensorType, store the \p outputShapesOrValues
  /// into VariableMeta.listOfShape;
  /// Else store the \p outputShapesOrValues into VariableMeta.intValue.
  /// For \p prim::ListConstruct, if the output is a Scalar[], Bool[],
  /// Store the shape of \p outputShapesOrValues into VariableMeta.listOfShape
  /// store the value of \p outputShapesOrValues into VariableMeta.intValue
  /// Else the output is Tensor[], Store the list of shape
  /// \p outputShapesOrValues into VariableMeta.listOfShape
  /// For \p aten::embedding_bag, since the output is a std::tuple<Tensor,
  /// Tensor, Tensor, Tensor>(ret, offset2bag, bag_size, bag_size), and for now,
  /// only the ret tensor shape needed, the embeddingBag() only generate the ret
  /// shape.
  /// For \p c10::aten::chunk, the output is tensor[],
  /// Store the shapes \p outputShapesOrValues into VariableMeta.listOfShape
  if (kind == c10::prim::Constant) {
    if (node->output()->type()->isSubtypeOf(at::TensorType::get())) {
      shapeMap_[node->output()].listOfShape.emplace_back(
          std::move(outputShapesOrValues[0]));
    } else {
      shapeMap_[node->output()].listOfShape.emplace_back((TensorShape){1});
      shapeMap_[node->output()].intValue = std::move(outputShapesOrValues[0]);
    }
  } else if (kind == c10::prim::ListConstruct) {
    auto elem_type =
        node->output()->type()->cast<c10::ListType>()->getElementType();
    if (elem_type->kind() == at::TensorType::Kind ||
        (elem_type->kind() == at::OptionalType::Kind &&
         elem_type->cast<c10::OptionalType>()->getElementType()->kind() ==
             at::TensorType::Kind)) {
      shapeMap_[node->output()].listOfShape.emplace_back(
          std::move(outputShapesOrValues));
    } else {
      shapeMap_[node->output()].listOfShape.emplace_back(
          (TensorShape){static_cast<long>(outputShapesOrValues[0].size()), 1});
      shapeMap_[node->output()].intValue = std::move(outputShapesOrValues[0]);
    }
  } else if (kind == c10::aten::embedding_bag) {
    shapeMap_[node->output(0)].listOfShape.emplace_back(
        std::move(outputShapesOrValues[0]));
  } else if (kind == c10::aten::chunk) {
    shapeMap_[node->output()].listOfShape.emplace_back(
        std::move(outputShapesOrValues));
  } else {
    for (int i = 0; i < node->outputs().size(); i++) {
      shapeMap_[node->output(i)].listOfShape.emplace_back(
          std::move(outputShapesOrValues[i]));
    }
  }
  return Error::success();
}

Error ShapeInferenceEngine::runRecursively(
    const torch::jit::Graph &graph,
    const at::ArrayRef<torch::jit::IValue> &inputs) {
  // Populate input shapes
  RETURN_IF_ERR(getGraphInputShape(graph, inputs));

  /// Run shape inference for each node
  for (auto *node : graph.nodes()) {
    if (node->hasAttribute(torch::jit::attr::Subgraph)) {
      std::string kind = node->kind().toQualString();
      CHECK_EQ(kind.find(fusionNodeSymbol_), 0);
      // After fusion the input Value of the subgraph and
      // input Value of the fusion node are different
      // in memory objects. Therefore we populate inputMeta
      // beforehand and pass it to recursive run.
      std::vector<torch::jit::IValue> subgraphInputs;
      for (auto i : node->inputs()) {
        auto it = shapeMap_.find(i);
        CHECK(it != shapeMap_.end());
        // Only support tensor input for now
        // TODO Add support for other input types, e.g., tensor list
        subgraphInputs.push_back(
            torch::empty(it->second.shape<TensorShape>(),
                         torch::TensorOptions().dtype(it->second.dtype)));
      }

      const at::ArrayRef<torch::jit::IValue> inputRefs(subgraphInputs);

      auto subgraph = node->g(torch::jit::attr::Subgraph);
      RETURN_IF_ERR(runRecursively(*subgraph, subgraphInputs));

      CHECK_EQ(subgraph->outputs().size(), node->outputs().size());
      for (int i = 0; i < subgraph->outputs().size(); ++i) {
        shapeMap_[node->outputs()[i]] = shapeMap_[subgraph->outputs()[i]];
      }
    } else {
      RETURN_IF_ERR(shapeOnNode(node));
    }
  }
  return Error::success();
}

Error ShapeInferenceEngine::run() {
  RETURN_ERR_IF_NOT(
      inputs_.size() == graph_.inputs().size(),
      "Number of inputs mismatch between Graph and actual inputs");

  /// Put graph input into shape mapping
  RETURN_IF_ERR(runRecursively(graph_, inputs_));

  /// Extract output from shape mapping
  generateGraphOutputShape();
  return Error::success();
}

void ShapeInferenceEngine::printShapeMap() {
  for (auto elem : shapeMap_) {
    std::cout << elem.first->debugName() << ":[ ";
    if (elem.second.listOfShape[0].type() == typeid(TensorShape)) {
      const TensorShape &shape = elem.second.shape<TensorShape>();
      for (auto value : shape) {
        std::cout << value << " ";
      }
    } else if (elem.second.listOfShape[0].type() == typeid(TensorListShape)) {
      const TensorListShape &shapes = elem.second.shape<TensorListShape>();
      for (auto shape : shapes) {
        std::cout << "[ ";
        for (auto value : shape) {
          std::cout << value << " ";
        }
        std::cout << "]";
      }
    } else {
      std::cout << "Type doesn't support yet.";
    }
    std::cout << "]" << std::endl;
  }
}

/// If the input is tensor, store the shape info only;
/// Else If the input is bool or int, store the value, and set shape as 1.
/// Else if the input is intlist, store the intlist, and set shape as [sizeof
/// intlist, 1]
/// Else return an error
Error ShapeInferenceEngine::getGraphInputShape(
    const torch::jit::Graph &graph,
    const at::ArrayRef<torch::jit::IValue> &inputs) {
  for (auto i = 0; i < inputs.size(); i++) {
    auto gInName = graph.inputs()[i];
    auto input = inputs[i];
    TensorShape shape = {};
    std::vector<int64_t> intValue = {};

    if (input.isTensor()) {
      auto ptTensor = input.toTensor();
      for (auto s : ptTensor.sizes()) {
        shape.emplace_back(s);
      }
    } else if (input.isBool() || input.isInt()) {
      shape = {1};
      intValue = {input.toInt()};
    } else if (input.isIntList()) {
      intValue = input.toIntVector();
      shape = {static_cast<long>(intValue.size()), 1};
    } else {
      return MAKE_ERR("Input type doesn't support yet.");
    }
    shapeMap_[gInName].listOfShape.emplace_back(std::move(shape));
    shapeMap_[gInName].intValue = intValue;
  }
  return Error::success();
}

void ShapeInferenceEngine::generateGraphOutputShape() {
  for (auto output : graph_.outputs()) {
    auto it = shapeMap_.find(output);
    CHECK(it != shapeMap_.end());
    outputShape_.emplace_back(it->second);
  }
}

/// The \p prim::Constant may have multiple types of output, eg.
/// int = prim::Constant[value=0]()
/// Float(1:1) = prim::Constant[value={0}]()
/// bool = prim::Constant[value=0]()
/// None = prim::Constant()
/// Tensor = prim::Constant[value= <Tensor>]()
/// If the output is a tensor, return shape info;
/// Else, return the value.
Expected<TensorShape>
ShapeInferenceEngine::primConstant(const torch::jit::Node *node) {

  TensorShape shapeOrValue;
  at::TypePtr type = node->output()->type();

  if (type->isSubtypeOf(at::FloatType::get())) {
    /// The float type will not affect the shape
    /// Set value as 1
    shapeOrValue = {1};
  } else if (type->isSubtypeOf(at::IntType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::BoolType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::NoneType::get())) {
    shapeOrValue = {};
  } else if (type->isSubtypeOf(at::TensorType::get())) {
    at::Tensor t = node->t(at::attr::value);
    for (auto s : t.sizes()) {
      shapeOrValue.emplace_back(s);
    }
  }
  return shapeOrValue;
}

/**
 * aten::add(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::pow(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::mul(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * variableMetas: 0: self, 1: other
 */
Expected<TensorShape>
ShapeInferenceEngine::binaryOp(const MetaStack &variableMetas) {

  if (variableMetas.size() != 2 && variableMetas.size() != 3) {
    return MAKE_ERR("Expected two or three inputs shapes of this operation.");
  }

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();
  const TensorShape &t1 = variableMetas[1].shape<TensorShape>();

  auto d0 = t0.size();
  auto d1 = t1.size();

  /// One input is Scalar
  if (d1 == 1) {
    return t0;
  }

  size_t dim = std::max(d0, d1);
  TensorShape shape(dim);

  for (auto i = 0; i < dim; i++) {
    auto j = -1 - i;
    if (i >= d0 || t0[d0 + j] == 1) {
      shape[dim + j] = t1[d1 + j];
    } else if (i >= d1 || t1[d1 + j] == 1) {
      shape[dim + j] = t0[d0 + j];
    } else {
      if (t1[d1 + j] != t0[d0 + j]) {
        return MAKE_ERR(
            strFormat("The size of tensor a (%zu) must match the size of "
                      "tensor b (%zu)at non-singleton dimension 1.",
                      t0[d0 + j], t1[d1 + j]));
      }

      shape[dim + j] = t1[d1 + j];
    }
  }
  return shape;
}

/**
 * aten::mm(Tensor self, Tensor mat2) -> Tensor
 * variableMetas: 0: self, 1: mat2
 */
Expected<TensorShape> ShapeInferenceEngine::mm(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(variableMetas.size() == 2,
                    "Expected two inputs shapes of this operation.");

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();
  const TensorShape &t1 = variableMetas[1].shape<TensorShape>();

  if (!(t1.size() == 2 && t0.size() == 2)) {
    return MAKE_ERR("Expected 2-dimensional tensor.");
  }

  if (t0[1] != t1[0]) {
    return MAKE_ERR(
        strFormat("The size of tensor a (%zu) at dimension 1 must match the "
                  "size of tensor b (%zu) at dimension 0.",
                  t0[1], t1[0]));
  }

  TensorShape shape = {t0[0], t1[1]};
  return shape;
}

/**
 * aten::bmm(Tensor self, Tensor mat2) -> Tensor
 * variableMetas: 0: self, 1: mat2
 */
Expected<TensorShape>
ShapeInferenceEngine::bmm(const MetaStack &variableMetas) {

  if (variableMetas.size() != 2) {
    return MAKE_ERR("Expected two inputs shapes of this operation.");
  }

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();
  const TensorShape &t1 = variableMetas[1].shape<TensorShape>();

  if (!(t0.size() == 3 && t1.size() == 3)) {
    return MAKE_ERR("Expected 3-dimensional tensor.");
  }

  if (t0[0] != t1[0]) {
    return MAKE_ERR("Expected tensors to have same size at dimension 0");
  }

  if (t0[2] != t1[1]) {
    return MAKE_ERR(strFormat("The size of tensor a (%zu) at dimension 2 must"
                              "match the size of tensor b (%zu) at dimension 1",
                              t0[2], t1[1]));
  }
  TensorShape shape = {t0[0], t0[1], t1[2]};
  return shape;
}

/**
 * aten::addmm(Tensor self, Tensor mat1, Tensor mat2, *, Scalar beta=1, Scalar
   alpha=1) -> Tensor
 * variableMetas: 0: self, 1: mat1, 2: mat2
 */
Expected<TensorShape>
ShapeInferenceEngine::addmm(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(variableMetas.size() >= 3,
                    strFormat("Expected at least three inputs shapes, got %zu.",
                              variableMetas.size()));

  const VariableMeta &t0 = variableMetas[0];
  const VariableMeta &t1 = variableMetas[1];
  const VariableMeta &t2 = variableMetas[2];
  VariableMeta t;

  // For Scalar type, the shape.size() is 1
  if (t2.shape<TensorShape>().size() == 1) {
    t = variableMetas[1];
  } else {
    const MetaStack &mmShape = {t1, t2};
    ElemShape s;
    ASSIGN_VALUE_OR_RETURN_ERR(s, mm(mmShape));
    t.listOfShape.emplace_back(std::move(s));
  }

  return binaryOp({t0, std::move(t)});
}

/**
 * aten::t(Tensor self) -> Tensor
 * refer to https://pytorch.org/docs/master/generated/torch.t
 */
Expected<TensorShape> ShapeInferenceEngine::t(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 1,
      strFormat("Expected one input, got %zu.", variableMetas.size()));

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();

  auto d0 = t0.size();

  /// 0-D or 1-D tensor: Same shape
  if (d0 == 1) {
    return t0;
    /// 2-D tensor: Transpose
  } else if (d0 == 2) {
    TensorShape shape{t0[1], t0[0]};
    return shape;
    /// >2-D tensor: Invalid input
  } else {
    return MAKE_ERR(strFormat("Expected tensor <= 2-D, got %zu-D.", d0));
  }
}

/**
 * aten::transpose(Tensor self, int dim0, int dim1) => Tensor
 * variableMetas: 0: self, 1: dim0, 2: dim1
 * refer to https://pytorch.org/docs/master/generated/torch.transpose
 **/
Expected<TensorShape>
ShapeInferenceEngine::transpose(const MetaStack &variableMetas) {
  if (variableMetas.size() != 3) {
    return MAKE_ERR(
        strFormat("Expect 3 inputs, get %zu", variableMetas.size()));
  }
  RETURN_ERR_IF_NOT(variableMetas[1].intValue.size() == 1,
                    "Expect 1 int dimension");
  RETURN_ERR_IF_NOT(variableMetas[2].intValue.size() == 1,
                    "Expect 1 int dimension");

  TensorShape shape = variableMetas[0].shape<TensorShape>();

  int64_t inDims = shape.size();
  int64_t dim0 = variableMetas[1].intValue[0];
  int64_t dim1 = variableMetas[2].intValue[0];

  // convert to positive dimension
  dim0 = at::maybe_wrap_dim(dim0, inDims);
  dim1 = at::maybe_wrap_dim(dim1, inDims);

  std::swap(shape[dim0], shape[dim1]);

  return shape;
}

/**
 * aten::cat(Tensors tensors, int dim=0) => Tensor
 * 0:variableMetas, 1: dim
 * refer to https://pytorch.org/docs/master/generated/torch.cat
 **/
Expected<TensorShape>
ShapeInferenceEngine::cat(const MetaStack &variableMetas) {
  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected 2 inputs, got %zu.", variableMetas.size()));

  const TensorListShape &tensorListShapes =
      variableMetas[0].shape<TensorListShape>();
  std::vector<int64_t> shape = tensorListShapes[0];

  // Hanlde the single input case
  if (tensorListShapes.size() == 1) {
    return shape;
  }

  // Convert negtive dimension to positive, then check the dim range.
  int64_t dim = variableMetas[1].intValue[0];
  int64_t inDims = shape.size();
  dim = at::maybe_wrap_dim(dim, inDims);

  // Handle multiple input cases.
  // Verify all inputs dimenions are the same execpt the dimension applies cat.
  for (int i = 1; i < tensorListShapes.size(); ++i) {
    RETURN_ERR_IF_NOT(inDims == tensorListShapes[i].size(),
                      "All inputs must have the same number of dimensions.");
    for (int j = 0; j < inDims; j++) {
      if (j == dim) {
        continue;
      } else {
        RETURN_ERR_IF_NOT(
            shape[j] == tensorListShapes[i][j],
            strFormat("Sizes of tensors must match except in dimension %zu.",
                      dim));
      }
    }
  }
  for (int i = 1; i < tensorListShapes.size(); ++i)
    shape[dim] += tensorListShapes[i][dim];

  return shape;
}

/**
 * aten::flatten(Tensor self, int start_dim, int end_dim) => Tensor
 * variableMetas: 0: self, 1: start_dim, 2: end_dim
 * refer to: https://pytorch.org/docs/master/generated/torch.flatten
 **/
Expected<TensorShape>
ShapeInferenceEngine::flatten(const MetaStack &variableMetas) {
  if (variableMetas.size() != 3) {
    return MAKE_ERR(
        strFormat("Expect 3 inputs, get %zu", variableMetas.size()));
  }
  RETURN_ERR_IF_NOT(variableMetas[1].intValue.size() == 1,
                    "Expect 1 int dimension");
  RETURN_ERR_IF_NOT(variableMetas[2].intValue.size() == 1,
                    "Expect 1 int dimension");

  const TensorShape &t = variableMetas[0].shape<TensorShape>();

  int64_t inDims = t.size();
  int64_t startDim = variableMetas[1].intValue[0];
  int64_t endDim = variableMetas[2].intValue[0];

  // convert to positive dimension
  startDim = at::maybe_wrap_dim(startDim, inDims);
  endDim = at::maybe_wrap_dim(endDim, inDims);

  if (startDim > endDim) {
    return MAKE_ERR("start dimension should not be larger than end dimension");
  }

  TensorShape shape;
  for (int i = 0; i < startDim; i++) {
    shape.push_back(t[i]);
  }
  int64_t flattenDim = 1;
  for (int i = startDim; i <= endDim; i++) {
    flattenDim *= t[i];
  }
  shape.push_back(flattenDim);
  for (int i = endDim + 1; i < inDims; i++) {
    shape.push_back(t[i]);
  }

  return shape;
}

/**
 * prim::ConstantChunk[int chunks, int dim](Tensor self) -> Tensors
 * variableMetas: 0: self
 */
Expected<TensorListShape>
ShapeInferenceEngine::constantChunk(const MetaStack &variableMetas,
                                    int64_t chunks, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 1,
      strFormat("Expected one input, got %zu.", variableMetas.size()));

  const TensorShape &t = variableMetas[0].shape<TensorShape>();

  /// Convert dim into positive
  int64_t inDims = t.size();
  dim = at::maybe_wrap_dim(dim, inDims);

  /// For constant chunk, the size of the last chunk one may smaller than the
  /// others
  int64_t c = (t[dim] + chunks - 1) / chunks;
  int64_t r = t[dim] - c * (chunks - 1);

  TensorListShape resShapes;
  for (int i = 0; i < chunks; i++) {
    TensorShape shape = t;
    shape[dim] = (i == chunks - 1) ? r : c;
    resShapes.emplace_back(shape);
  }

  return resShapes;
}

/**
 * prim::FusedConcat[int dim](Tensor self, Tensor mat1, Tensor mat2, ...) ->
 * Tensor variableMetas: 0: self, 1: mat1, 2: mat2, ...
 */
Expected<TensorShape>
ShapeInferenceEngine::fusedConcat(const MetaStack &variableMetas, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  if (variableMetas.size() == 1) {
    return variableMetas[0].shape<TensorShape>();
  }

  TensorShape shape = variableMetas[0].shape<TensorShape>();
  /// Convert negtive dimension to positive, then check the dim range.
  int64_t inDims = shape.size();
  dim = at::maybe_wrap_dim(dim, inDims);

  /// Handle multiple inputs cases
  for (int i = 1; i < variableMetas.size(); ++i) {
    const TensorShape &t = variableMetas[i].shape<TensorShape>();
    RETURN_ERR_IF_NOT(inDims == t.size(),
                      "All inputs must have the same number of dimensions.");
    for (int j = 0; j < inDims; j++) {
      if (j == dim) {
        shape[dim] += t[dim];
      } else {
        RETURN_ERR_IF_NOT(
            shape[j] == t[j],
            strFormat("Sizes of tensors must match except in dimension %zu.",
                      dim));
      }
    }
  }
  return shape;
}

/**
 * aten::slice(Tensor self, int dim, int start, int end, int step)
 * variableMetas: 0: self, 1: dim, 2: start, 3: end, 4: step.
 */
Expected<TensorShape>
ShapeInferenceEngine::slice(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 5,
      strFormat("Expected 5 inputs, got %zu.", variableMetas.size()));

  for (int i = 1; i < 5; i++) {
    RETURN_ERR_IF_NOT(variableMetas[i].intValue.size() == 1,
                      "Expected int in Slice.");
  }

  int64_t dim = variableMetas[1].intValue[0];
  int64_t start = variableMetas[2].intValue[0];
  int64_t end = variableMetas[3].intValue[0];
  int64_t step = variableMetas[4].intValue[0];

  TensorShape shape = variableMetas[0].shape<TensorShape>();
  int64_t inDims = shape[dim];

  /// Check if the start or end dim out of the input dimension
  if (start >= inDims || end <= -inDims) {
    shape[dim] = 0;
    return shape;
  }

  /// Convert start dim into positive
  if (start <= -inDims) {
    start = 0;
  } else if (start > -inDims && start < 0) {
    start += inDims;
  }

  /// Convert end dim into positive
  if (end > inDims) {
    end = inDims;
  } else if (end > -inDims && end < 0) {
    end += inDims;
  }

  if (start >= end) {
    shape[dim] = 0;
    return shape;
  }

  shape[dim] = (end - start) / step;
  if ((end - start) % step) {
    shape[dim] += 1;
  }
  return shape;
}

/**
 * aten::reshape(Tensor self, int[] shape) -> Tensor
 * variableMetas: 0: self, 1: shape
 */
Expected<TensorShape>
ShapeInferenceEngine::reshape(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected two inputs shapes, got %zu.", variableMetas.size()));

  int64_t s0 = 1;
  int64_t s1 = 1;

  const TensorShape &t = variableMetas[0].shape<TensorShape>();

  /// Flag for multiple negative index
  int64_t negIndex = -1;
  for (auto i : t) {
    s0 *= i;
  }

  for (int i = 0; i < variableMetas[1].intValue.size(); i++) {
    s1 *= variableMetas[1].intValue[i];
    if (variableMetas[1].intValue[i] == -1) {
      if (negIndex == -1) {
        negIndex = i;
      } else {
        return MAKE_ERR("Unable to infer undetermined dimension");
      }
    }
  }

  RETURN_ERR_IF_NOT(s0 % s1 == 0, "Reshape size is invalid for input size.");

  TensorShape shape = variableMetas[1].intValue;

  if (negIndex != -1) {
    shape[negIndex] = -s0 / s1;
  }
  return shape;
}

/**
 * aten::permute(Tensor self, int[] shape) -> Tensor
 * variableMetas: 0: self, 1: shape
 */
Expected<TensorShape>
ShapeInferenceEngine::permute(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected two inputs shapes, got %zu.", variableMetas.size()));

  const TensorShape &t = variableMetas[0].shape<TensorShape>();

  int64_t inDims = t.size();

  RETURN_ERR_IF_NOT(inDims == variableMetas[1].intValue.size(),
                    "Shuffle for permute must has the same number of "
                    "dimensions as the input tensor.");

  TensorShape shape;

  for (int64_t dim : variableMetas[1].intValue) {
    RETURN_ERR_IF_NOT(dim >= 0,
                      "Negative shuffle dimensions not supported by Glow yet.");
    RETURN_ERR_IF_NOT(
        dim < inDims,
        "All shuffle dimensions must be less than the rank of the input.");
    shape.emplace_back(t[dim]);
  }
  return shape;
}

/**
 * prim::ListContruct(Scalar/Bool/Tensor self, Scalar/Bool/Tensor v1,
 * Scalar/Bool/Tensor v2, ...) -> Scalar[]/Bool[]/Tensor[]
 * variableMetas: 0: self, 1: v1, 2: v2, ...
 */
Expected<TensorListShape>
ShapeInferenceEngine::listConstruct(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  TensorListShape listValueOrShape(1);
  if (variableMetas[0].intValue.size() == 1) {
    // scalar or bool
    for (auto ele : variableMetas) {
      RETURN_ERR_IF_NOT(ele.intValue.size() == 1,
                        "Expected int type input in listConstruct.");
      listValueOrShape[0].emplace_back(ele.intValue[0]);
    }
  } else {
    // tensor
    listValueOrShape.resize(variableMetas.size());
    for (int i = 0; i < variableMetas.size(); i++) {
      listValueOrShape[i] = variableMetas[i].shape<TensorShape>();
    }
  }
  return listValueOrShape;
}

/**
 * glow::fused_stack[dim=1](Tensor self, Tensor mat1, Tensor mat2, ...)
 * variableMetas: 0: self, 1: mat1, 2: mat2, ...
 */
Expected<TensorShape>
ShapeInferenceEngine::fusedStack(const MetaStack &variableMetas, int64_t dim) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", variableMetas.size()));

  TensorShape shape = variableMetas[0].shape<TensorShape>();

  if (variableMetas.size() == 1) {
    return shape;
  }
  int64_t inDims = shape.size();
  /// glow::fused_stack will add one more dim
  dim = at::maybe_wrap_dim(dim, inDims + 1);

  for (auto eleShape : variableMetas) {
    RETURN_ERR_IF_NOT(eleShape.shape<TensorShape>() == shape,
                      "All inputs must have same shape");
  }

  shape.insert(shape.begin() + dim, variableMetas.size());
  return shape;
}

/**
 * aten::_embedding_bag(Tensor weight,
 *                      Tensor indices,
 *                      Tensor offsets,
 *                      bool scale_grad_by_freq=False,
 *                      int mode=0,
 *                      bool sparse=False,
 *                      Tensor? per_sample_weights=None,
 *                      bool include_last_offset=False)
 *                      -> (Tensor, Tensor, Tensor, Tensor)
 */
/// Since the first output tensor is the result, and we only need the shape of
/// result Return the shape of the first tensor only
/// In glow, the include_last_offset is always True.
Expected<TensorShape>
ShapeInferenceEngine::embeddingBag(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 8,
      strFormat("Expected 8 inputs, got %zu.", variableMetas.size()));

  TensorShape shape;

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();

  const TensorShape &t1 = variableMetas[1].shape<TensorShape>();

  const TensorShape &t2 = variableMetas[2].shape<TensorShape>();

  if (t1.size() == 1) {
    RETURN_ERR_IF_NOT(t2.size() == 1,
                      strFormat("Expected 1D offset, got %zu.", t2.size()));
    shape = {t2[0] - static_cast<int>(((hasEndOffset_) ? 1 : 0)), t0[1]};
  } else if (t1.size() == 2) {
    shape = {t1[0], t0[1]};
  } else {
    return MAKE_ERR("Only support 1D and 2D Input in Embedding bag.");
  }
  return shape;
}

/**
 * fb::embedding_bag_byte_rowwise_offsets(Tensor weight,
 *                                        Tensor indices,
 *                                        Tensor offsets,
 *                                        bool scale_grad_by_freq=False,
 *                                        int mode=0,
 *                                        bool sparse=False,
 *                                        Tensor? per_sample_weights=None,
 *                                        bool include_last_offset=True)
 *                                        -> Tensor;
 */
/// In glow, the include_last_offset is always True.
Expected<TensorShape> ShapeInferenceEngine::embeddingBagByteRowwiseOffsets(
    const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 8,
      strFormat("Expected 8 inputs, got %zu.", variableMetas.size()));

  const TensorShape &t0 = variableMetas[0].shape<TensorShape>();

  const TensorShape &t2 = variableMetas[2].shape<TensorShape>();

  /// variableMetas[0].shape[1] - 8 is to account for scale and bias
  /// 4-byte scale, 4-byte zero_offset
  TensorShape shape = {t2[0] - static_cast<int>(((hasEndOffset_) ? 1 : 0)),
                       t0[1] - 8};
  return shape;
}

/**
 * aten::chuck(Tensor self, int chunks, int dim) -> Tensor[]
 * refer to: https://pytorch.org/docs/master/generated/torch.chunk
 */
Expected<TensorListShape>
ShapeInferenceEngine::chunk(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 3,
      strFormat("Expected one input, got %zu.", variableMetas.size()));

  const TensorShape &t = variableMetas[0].shape<TensorShape>();

  int64_t chunks = variableMetas[1].intValue[0];
  int64_t dim = variableMetas[2].intValue[0];

  /// Convert dim into positive
  int64_t inDims = t.size();
  dim = at::maybe_wrap_dim(dim, inDims);

  /// For constant chunk, the size of the last chunk one may smaller than the
  /// others
  int64_t c = (t[dim] + chunks - 1) / chunks;
  int64_t r = t[dim] - c * (chunks - 1);

  TensorListShape resShapes;
  for (int i = 0; i < chunks; i++) {
    TensorShape shape = t;
    shape[dim] = (i == chunks - 1) ? r : c;
    resShapes.emplace_back(shape);
  }
  return resShapes;
}

/*
 * fb::embedding_bag_4bit_rowwise_offsets(Tensor weight,
 *                                        Tensor indices,
 *                                        Tensor offsets,
 *                                        bool scale_grad_by_freq=False,
 *                                        int mode=0,
 *                                        bool sparse=False,
 *                                        Tensor? per_sample_weights=None,
 *                                        Tensor? compressed_indices_mapping,
 *                                        bool include_last_offset=True)
 *                                        -> Tensor;
 */
/// In glow, the include_last_offset is always True.
Expected<TensorShape> ShapeInferenceEngine::embeddingBag4BitRowwiseOffsets(
    const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 9,
      strFormat("Expected 9 inputs, got %zu.", variableMetas.size()));

  /// variableMetas[0].shape[1] - 4 is to account for scale and offsets
  /// Note: 2-byte fp16 scale and 2-byte zero_offset
  /// *2 which accounts for the packed fp16 weights
  const TensorShape &weightShape = variableMetas[0].shape<TensorShape>();
  const TensorShape &offsetsShape = variableMetas[2].shape<TensorShape>();
  TensorShape shape = {offsetsShape[0] -
                           static_cast<int>(((hasEndOffset_) ? 1 : 0)),
                       (weightShape[1] - 4) * 2};
  return shape;
}

/**
 * aten::stack(Tensor[] tensors, int dim) -> Tensor
 * refer to: https://pytorch.org/docs/stable/generated/torch.stack
 */
Expected<TensorShape>
ShapeInferenceEngine::stack(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 2,
      strFormat("Expected 2 input, got %zu.", variableMetas.size()));

  const TensorListShape &shapes = variableMetas[0].shape<TensorListShape>();
  TensorShape shape = shapes[0];

  // Convert negtive dimension to positive, then check the dim range.
  int64_t dim = variableMetas[1].intValue[0];
  int64_t inDims = shape.size();
  dim = at::maybe_wrap_dim(dim, inDims);

  // Verify the shapes of all input tensors.
  for (int i = 1; i < shapes.size(); i++) {
    RETURN_ERR_IF_NOT(shape == shapes[i],
                      "All tensors need to be of the same shape.");
  }

  shape.insert(shape.begin() + dim, shapes.size());
  return shape;
}

/**
 * prim::ListUnpack(Tensor[] tensors) -> Tensor, ..., Tensor
 */
Expected<std::vector<TensorShape>>
ShapeInferenceEngine::listUnpack(const MetaStack &variableMetas) {

  RETURN_ERR_IF_NOT(
      variableMetas.size() == 1,
      strFormat("Expected 1 input, got %zu.", variableMetas.size()));

  std::vector<TensorShape> shapes;
  const TensorListShape &t = variableMetas[0].shape<TensorListShape>();

  for (int i = 0; i < t.size(); i++) {
    shapes.emplace_back(t[i]);
  }

  return shapes;
}
} // namespace glow
