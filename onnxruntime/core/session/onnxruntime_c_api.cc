// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/session/onnxruntime_c_api.h"
#include "core/session/allocator_impl.h"
#include "core/framework/error_code_helper.h"
#include "core/framework/execution_provider.h"
#include <cassert>
#include <cstring>
#include <sstream>

#include "core/common/logging/logging.h"
#include "core/common/logging/sinks/clog_sink.h"
#include "core/common/status.h"
#include "core/graph/graph.h"
#include "core/framework/allocator.h"
#include "core/framework/tensor.h"
#include "core/framework/ml_value.h"
#include "core/session/environment.h"
#include "core/common/callback.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/onnxruntime_typeinfo.h"
#include "core/session/inference_session.h"
#include "core/framework/data_types.h"
#include "abi_session_options_impl.h"

using namespace onnxruntime::logging;
using onnxruntime::BFloat16;
using onnxruntime::DataTypeImpl;
using onnxruntime::Environment;
using onnxruntime::IAllocator;
using onnxruntime::InputDefList;
using onnxruntime::MLFloat16;
using onnxruntime::MLStatus;
using onnxruntime::OutputDefList;
using onnxruntime::Tensor;
using onnxruntime::ToOrtStatus;
using onnxruntime::common::Status;

using namespace onnxruntime;

#define ORT_API_RETURN_IF_ERROR(expr) \
  do {                                \
    auto _status = (expr);            \
    if (_status) return _status;      \
  } while (0)

struct OrtEnv {
 public:
  Environment* value;
  LoggingManager* loggingManager;

  OrtEnv(Environment* value1, LoggingManager* loggingManager1) : value(value1), loggingManager(loggingManager1) {
  }
  /**
   * This function will call ::google::protobuf::ShutdownProtobufLibrary
   */
  ~OrtEnv() {
    delete loggingManager;
    delete value;
  }
  ORT_DISALLOW_COPY_AND_ASSIGNMENT(OrtEnv);
};

#define TENSOR_READ_API_BEGIN                          \
  API_IMPL_BEGIN                                       \
  auto v = reinterpret_cast<const ::OrtValue*>(value); \
  auto& tensor = v->Get<onnxruntime::Tensor>();

#define TENSOR_READWRITE_API_BEGIN               \
  API_IMPL_BEGIN                                 \
  auto v = reinterpret_cast<::OrtValue*>(value); \
  auto tensor = v->GetMutable<onnxruntime::Tensor>();

class LoggingWrapper : public ISink {
 public:
  LoggingWrapper(OrtLoggingFunction logging_function, void* logger_param)
      : logging_function_{logging_function}, logger_param_{logger_param} {
  }

  void SendImpl(const Timestamp& /*timestamp*/ /*timestamp*/, const std::string& logger_id,
                const Capture& message) override {
    std::string s = message.Location().ToString();
    logging_function_(logger_param_, static_cast<OrtLoggingLevel>(message.Severity()), message.Category(),
                      logger_id.c_str(), s.c_str(), message.Message().c_str());
  }

 private:
  OrtLoggingFunction logging_function_;
  void* logger_param_;
};

ORT_API_STATUS_IMPL(OrtCreateEnvWithCustomLogger, OrtLoggingFunction logging_function,
                    _In_opt_ void* logger_param, OrtLoggingLevel default_warning_level, _In_ const char* logid,
                    _Out_ OrtEnv** out) {
  API_IMPL_BEGIN
  std::string name = logid;
  std::unique_ptr<ISink> logger = std::make_unique<LoggingWrapper>(logging_function, logger_param);
  auto default_logging_manager = std::make_unique<LoggingManager>(std::move(logger),
                                                                  static_cast<Severity>(default_warning_level), false,
                                                                  LoggingManager::InstanceType::Default,
                                                                  &name);
  std::unique_ptr<Environment> env;
  Status status = Environment::Create(env);
  if (status.IsOK())
    *out = new OrtEnv(env.release(), default_logging_manager.release());
  return ToOrtStatus(status);
  API_IMPL_END
}

ORT_API(const char*, OrtGetVersionString) {
  return ORT_VERSION;
}

ORT_API_STATUS_IMPL(OrtCreateEnv, OrtLoggingLevel default_warning_level,
                    _In_ const char* logid, _Out_ OrtEnv** out) {
  API_IMPL_BEGIN
  std::string name = logid;
  auto default_logging_manager = std::make_unique<LoggingManager>(std::unique_ptr<ISink>{new CLogSink{}},
                                                                  static_cast<Severity>(default_warning_level), false,
                                                                  LoggingManager::InstanceType::Default,
                                                                  &name);
  std::unique_ptr<Environment> env;
  Status status = Environment::Create(env);
  if (status.IsOK()) {
    *out = new OrtEnv(env.release(), default_logging_manager.release());
    return nullptr;
  }
  *out = nullptr;
  return ToOrtStatus(status);
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtGetStringTensorDataLength, _In_ const OrtValue* value, _Out_ size_t* out) {
  TENSOR_READ_API_BEGIN
  const auto* src = tensor.Data<std::string>();
  int64_t len = tensor.Shape().Size();
  if (len >= 0) {
    size_t ret = 0;
    for (int64_t i = 0; i != len; ++i) {
      ret += src[i].size();
    }
    *out = ret;
  } else
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, "shape is invalid");
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtFillStringTensor, _In_ OrtValue* value, _In_ const char* const* s, size_t s_len) {
  TENSOR_READWRITE_API_BEGIN
  auto* dst = tensor->MutableData<std::string>();
  auto len = static_cast<size_t>(tensor->Shape().Size());
  if (s_len < len) {
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, "input array is too short");
  }
  for (size_t i = 0; i != len; ++i) {
    //allocate and copy
    dst[i] = s[i];
  }
  return nullptr;
  API_IMPL_END
}

template <typename T>
OrtStatus* CreateTensorImpl(const int64_t* shape, size_t shape_len, OrtAllocator* allocator,
                            std::unique_ptr<Tensor>* out) {
  std::vector<int64_t> shapes(shape_len);
  for (size_t i = 0; i != shape_len; ++i) {
    shapes[i] = shape[i];
  }
  std::shared_ptr<IAllocator> alloc_ptr = std::make_shared<onnxruntime::AllocatorWrapper>(allocator);
  *out = std::make_unique<Tensor>(DataTypeImpl::GetType<T>(), onnxruntime::TensorShape(shapes), alloc_ptr);
  return nullptr;
}

/**
 *
 * this function will create a copy of the allocator info
 */
template <typename T>
OrtStatus* CreateTensorImpl(const int64_t* shape, size_t shape_len, const OrtAllocatorInfo* info,
                            void* p_data, size_t p_data_len, std::unique_ptr<Tensor>* out) {
  TensorShape tensor_shape(shape, shape_len);
  int64_t elem_count = tensor_shape.Size();
  if (elem_count < 0 || static_cast<uint64_t>(elem_count) > std::numeric_limits<size_t>::max()) {
    std::ostringstream oss;
    oss << "Create tensor failed. Tensor shape:" << shape[0];
    for (size_t i = 1; i != shape_len; ++i) {
      oss << "," << shape[i];
    }
    oss << " is invalid";
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, oss.str().c_str());
  }

  size_t size_to_allocate;
  if (!IAllocator::CalcMemSizeForArray(sizeof(T), static_cast<size_t>(elem_count), &size_to_allocate)) {
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, "size overflow");
  }
  if (size_to_allocate > p_data_len) {
    std::ostringstream oss;
    oss << "Create tensor failed. The preallocated buffer is not large enough: expected " << size_to_allocate
        << " bytes, got " << p_data_len << ".";
    if (shape_len > 0) {
      oss << " Tensor shape: [" << shape[0];
      for (size_t i = 1; i != shape_len; ++i) {
        oss << "," << shape[i];
      }
      oss << "].";
    }
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, oss.str().c_str());
  }
  *out = std::make_unique<Tensor>(DataTypeImpl::GetType<T>(), tensor_shape, p_data, *info);
  return nullptr;
}

/**
 * this function will create a copy of the allocator info
 */
ORT_API_STATUS_IMPL(OrtCreateTensorWithDataAsOrtValue, _In_ const OrtAllocatorInfo* info,
                    _Inout_ void* p_data, size_t p_data_len, _In_ const int64_t* shape, size_t shape_len,
                    ONNXTensorElementDataType type, _Out_ OrtValue** out) {
  API_IMPL_BEGIN
  std::unique_ptr<Tensor> tensor;
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<float>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint8_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int8_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint16_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int16_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int32_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int64_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<std::string>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<bool>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<MLFloat16>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<BFloat16>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<double>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint32_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint64_t>(shape, shape_len, info, p_data, p_data_len, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64:
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128:
    default: {
      std::ostringstream oss;
      oss << "type " << type << " is not supported in this function";
      std::string errmsg = oss.str();
      return OrtCreateStatus(ORT_NOT_IMPLEMENTED, errmsg.c_str());
    }
  }
  auto value = std::make_unique<OrtValue>();
  value->Init(tensor.release(),
              DataTypeImpl::GetType<Tensor>(),
              DataTypeImpl::GetType<Tensor>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtCreateTensorAsOrtValue, _Inout_ OrtAllocator* allocator,
                    _In_ const int64_t* shape, size_t shape_len, ONNXTensorElementDataType type,
                    _Out_ OrtValue** out) {
  API_IMPL_BEGIN
  std::unique_ptr<Tensor> tensor;
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<float>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint8_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int8_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint16_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int16_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int32_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<int64_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<std::string>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<bool>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<MLFloat16>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<BFloat16>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<double>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint32_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
      ORT_API_RETURN_IF_ERROR(CreateTensorImpl<uint64_t>(shape, shape_len, allocator, &tensor));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64:
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128:
    default: {
      std::ostringstream oss;
      oss << "type " << type << " is not supported in this function";
      std::string errmsg = oss.str();
      return OrtCreateStatus(ORT_NOT_IMPLEMENTED, errmsg.c_str());
    }
  }
  auto value = std::make_unique<OrtValue>();
  value->Init(tensor.release(),
              DataTypeImpl::GetType<Tensor>(),
              DataTypeImpl::GetType<Tensor>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtCreateCustomOpDomain, _In_ const char* domain, _Out_ OrtCustomOpDomain** out) {
  API_IMPL_BEGIN
  auto custom_op_domain = std::make_unique<OrtCustomOpDomain>();
  custom_op_domain->domain_ = domain;
  *out = custom_op_domain.release();
  return nullptr;
  API_IMPL_END
}

ORT_API(void, OrtReleaseCustomOpDomain, OrtCustomOpDomain* ptr) {
  delete ptr;
}

ORT_API_STATUS_IMPL(OrtCustomOpDomain_Add, _In_ OrtCustomOpDomain* custom_op_domain, OrtCustomOp* op) {
  API_IMPL_BEGIN
  custom_op_domain->custom_ops_.emplace_back(op);
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtAddCustomOpDomain, _In_ OrtSessionOptions* options, OrtCustomOpDomain* custom_op_domain) {
  API_IMPL_BEGIN
  options->custom_op_domains_.emplace_back(custom_op_domain);
  return nullptr;
  API_IMPL_END
}

namespace {
template <typename Loader>
OrtStatus* CreateSessionImpl(_In_ OrtEnv* env, _In_ const OrtSessionOptions* options,
                             Loader loader, _Out_ OrtSession** out) {
  auto sess = std::make_unique<::onnxruntime::InferenceSession>(
      options == nullptr ? onnxruntime::SessionOptions() : options->value, env->loggingManager);
  Status status;
  if (options != nullptr) {
    if (!options->custom_op_domains_.empty()) {
      status = sess->AddCustomOpDomains(options->custom_op_domains_);
      if (!status.IsOK())
        return ToOrtStatus(status);
    }
  }

  if (options != nullptr)
    for (auto& factory : options->provider_factories) {
      auto provider = factory->CreateProvider();
      if (provider)
        sess->RegisterExecutionProvider(std::move(provider));
    }
  status = loader(*sess);
  if (!status.IsOK())
    return ToOrtStatus(status);
  status = sess->Initialize();
  if (!status.IsOK())
    return ToOrtStatus(status);
  *out = reinterpret_cast<OrtSession*>(sess.release());
  return nullptr;
}
}  // namespace

ORT_API_STATUS_IMPL(OrtCreateSession, _In_ OrtEnv* env, _In_ const ORTCHAR_T* model_path,
                    _In_ const OrtSessionOptions* options, _Out_ OrtSession** out) {
  API_IMPL_BEGIN
  const auto loader = [model_path](InferenceSession& sess) {
    return sess.Load(model_path);
  };
  return CreateSessionImpl(env, options, loader, out);
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtCreateSessionFromArray, _In_ OrtEnv* env, _In_ const void* model_data, size_t model_data_length,
                    _In_ const OrtSessionOptions* options, _Out_ OrtSession** out) {
  API_IMPL_BEGIN
  const auto loader = [model_data, model_data_length](InferenceSession& sess) {
    return sess.Load(model_data, static_cast<int>(model_data_length));
  };
  return CreateSessionImpl(env, options, loader, out);
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtRun, _In_ OrtSession* sess,
                    _In_ const OrtRunOptions* run_options,
                    _In_ const char* const* input_names, _In_ const OrtValue* const* input, size_t input_len,
                    _In_ const char* const* output_names1, size_t output_names_len, _Out_ OrtValue** output) {
  API_IMPL_BEGIN
  auto session = reinterpret_cast<::onnxruntime::InferenceSession*>(sess);
  const int queue_id = 0;

  std::vector<std::string> feed_names(input_len);
  std::vector<OrtValue> feeds(input_len);

  for (size_t i = 0; i != input_len; ++i) {
    if (input_names[i] == nullptr || input_names[i][0] == '\0') {
      return OrtCreateStatus(ORT_INVALID_ARGUMENT, "input name cannot be empty");
    }

    feed_names[i] = input_names[i];
    auto& ort_value = feeds[i] = *reinterpret_cast<const ::OrtValue*>(input[i]);

    if (ort_value.Fence()) ort_value.Fence()->BeforeUsingAsInput(onnxruntime::kCpuExecutionProvider, queue_id);
  }

  // Create output feed
  std::vector<std::string> output_names(output_names_len);
  for (size_t i = 0; i != output_names_len; ++i) {
    if (output_names1[i] == nullptr || output_names1[i][0] == '\0') {
      return OrtCreateStatus(ORT_INVALID_ARGUMENT, "output name cannot be empty");
    }
    output_names[i] = output_names1[i];
  }

  std::vector<OrtValue> fetches(output_names_len);
  for (size_t i = 0; i != output_names_len; ++i) {
    if (output[i] != nullptr) {
      ::OrtValue& value = *reinterpret_cast<::OrtValue*>(output[i]);
      if (value.Fence())
        value.Fence()->BeforeUsingAsOutput(onnxruntime::kCpuExecutionProvider, queue_id);
      fetches[i] = value;
    }
  }
  Status status;
  if (run_options == nullptr) {
    OrtRunOptions op;
    status = session->Run(op, feed_names, feeds, output_names, &fetches);
  } else {
    status = session->Run(*run_options, feed_names, feeds, output_names, &fetches);
  }

  if (!status.IsOK())
    return ToOrtStatus(status);
  for (size_t i = 0; i != output_names_len; ++i) {
    ::OrtValue& value = fetches[i];
    if (value.Fence())
      value.Fence()->BeforeUsingAsInput(onnxruntime::kCpuExecutionProvider, queue_id);
    if (output[i] == nullptr) {
      output[i] = new OrtValue(value);
    }
  }
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtGetTensorMutableData, _In_ OrtValue* value, _Out_ void** output) {
  TENSOR_READWRITE_API_BEGIN
  //TODO: test if it's a string tensor
  *output = tensor->MutableDataRaw();
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtGetStringTensorContent, _In_ const OrtValue* value,
                    _Out_ void* s, size_t s_len, _Out_ size_t* offsets, size_t offsets_len) {
  TENSOR_READ_API_BEGIN
  const auto* input = tensor.Data<std::string>();
  auto len = static_cast<size_t>(tensor.Shape().Size());
  if (offsets_len < len) {
    return OrtCreateStatus(ORT_FAIL, "space is not enough");
  }
  {
    size_t ret = 0;
    for (size_t i = 0; i != len; ++i) {
      ret += input[i].size();
    }
    if (s_len < ret) {
      return OrtCreateStatus(ORT_FAIL, "space is not enough");
    }
  }
  size_t f = 0;
  char* p = static_cast<char*>(s);
  for (size_t i = 0; i != offsets_len; ++i, ++offsets) {
    memcpy(p, input[i].data(), input[i].size());
    p += input[i].size();
    *offsets = f;
    f += input[i].size();
  }
  return nullptr;
  API_IMPL_END
}

#define ORT_C_API_RETURN_IF_ERROR(expr)                 \
  do {                                                  \
    auto _status = (expr);                              \
    if ((!_status.IsOK())) return ToOrtStatus(_status); \
  } while (0)

ORT_API_STATUS_IMPL(OrtTensorProtoToOrtValue, _In_ const void* input, int input_len,
                    _In_opt_ const ORTCHAR_T* input_file_path, _Inout_ void* preallocated, size_t preallocated_size,
                    _Out_ OrtValue** out, _Out_ OrtCallback** deleter) {
  API_IMPL_BEGIN
  OrtAllocatorInfo* cpuAllocatorInfo;
  auto st = OrtCreateCpuAllocatorInfo(OrtDeviceAllocator, OrtMemTypeDefault, &cpuAllocatorInfo);
  if (st != nullptr) return st;
  ::ONNX_NAMESPACE::TensorProto proto;
  if (!proto.ParseFromArray(input, input_len)) {
    return OrtCreateStatus(ORT_FAIL, "parse input tensor proto failed");
  }
  auto value = std::make_unique<OrtValue>();
  std::unique_ptr<OrtCallback> del = std::make_unique<OrtCallback>();
  auto status =
      utils::TensorProtoToMLValue(Env::Default(), input_file_path, proto,
                                  MemBuffer(preallocated, preallocated_size, *cpuAllocatorInfo), *value, *del);
  OrtReleaseAllocatorInfo(cpuAllocatorInfo);
  if (!status.IsOK()) {
    return ToOrtStatus(status);
  }
  *out = value.release();
  if (del->f != nullptr) {
    *deleter = del.release();
  } else
    *deleter = nullptr;
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtGetTensorMemSizeInBytesFromTensorProto, _In_ const void* input, int input_len, size_t alignment,
                    size_t* out) {
  API_IMPL_BEGIN
  ::ONNX_NAMESPACE::TensorProto proto;
  if (!proto.ParseFromArray(input, input_len)) {
    return OrtCreateStatus(ORT_FAIL, "parse input tensor proto failed");
  }
  switch (alignment) {
    case 0:
      ORT_C_API_RETURN_IF_ERROR(utils::GetSizeInBytesFromTensorProto<0>(proto, out));
      break;
    case 256:
      ORT_C_API_RETURN_IF_ERROR(utils::GetSizeInBytesFromTensorProto<256>(proto, out));
      break;
    default:
      return OrtCreateStatus(ORT_INVALID_ARGUMENT, "Invalid alignment, which can only be 0 or 256");
  }
  return nullptr;
  API_IMPL_END
}
#define DEFINE_RELEASE_ORT_OBJECT_FUNCTION(INPUT_TYPE, REAL_TYPE) \
  ORT_API(void, OrtRelease##INPUT_TYPE, Ort##INPUT_TYPE* value) { \
    delete reinterpret_cast<REAL_TYPE*>(value);                   \
  }

ORT_API_STATUS_IMPL(OrtSessionGetInputCount, _In_ const OrtSession* sess, _Out_ size_t* out) {
  API_IMPL_BEGIN
  auto session = reinterpret_cast<const ::onnxruntime::InferenceSession*>(sess);
  std::pair<Status, const InputDefList*> p = session->GetModelInputs();
  if (!p.first.IsOK())
    return ToOrtStatus(p.first);
  *out = p.second->size();
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtSessionGetOutputCount, _In_ const OrtSession* sess, _Out_ size_t* out) {
  API_IMPL_BEGIN
  auto session = reinterpret_cast<const ::onnxruntime::InferenceSession*>(sess);
  std::pair<Status, const InputDefList*> p = session->GetModelOutputs();
  if (!p.first.IsOK())
    return ToOrtStatus(p.first);
  *out = p.second->size();
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtSessionGetInputTypeInfo, _In_ const OrtSession* sess, size_t index, _Out_ struct OrtTypeInfo** out) {
  API_IMPL_BEGIN
  auto session = reinterpret_cast<const ::onnxruntime::InferenceSession*>(sess);
  std::pair<Status, const InputDefList*> p = session->GetModelInputs();
  if (!p.first.IsOK())
    return ToOrtStatus(p.first);
  if (p.second->size() <= index)
    return OrtCreateStatus(ORT_FAIL, "out of index");
  const ONNX_NAMESPACE::TypeProto* type_proto = (*p.second)[index]->TypeAsProto();
  return OrtTypeInfo::FromDataTypeImpl(type_proto, out);
  API_IMPL_END
}
ORT_API_STATUS_IMPL(OrtSessionGetOutputTypeInfo, _In_ const OrtSession* sess, size_t index, _Out_ struct OrtTypeInfo** out) {
  API_IMPL_BEGIN
  auto session = reinterpret_cast<const ::onnxruntime::InferenceSession*>(sess);
  std::pair<Status, const InputDefList*> p = session->GetModelOutputs();
  if (!p.first.IsOK())
    return ToOrtStatus(p.first);
  if (p.second->size() <= index)
    return OrtCreateStatus(ORT_FAIL, "out of index");
  const ONNX_NAMESPACE::TypeProto* type_proto = (*p.second)[index]->TypeAsProto();
  return OrtTypeInfo::FromDataTypeImpl(type_proto, out);
  API_IMPL_END
}

static char* StrDup(const std::string& str, OrtAllocator* allocator) {
  char* output_string = reinterpret_cast<char*>(allocator->Alloc(allocator, str.size() + 1));
  memcpy(output_string, str.c_str(), str.size());
  output_string[str.size()] = '\0';
  return output_string;
}

static OrtStatus* GetInputOutputNameImpl(_In_ const OrtSession* sess, size_t index,
                                         _Inout_ OrtAllocator* allocator, bool is_input,
                                         _Out_ char** output) {
  auto session = reinterpret_cast<const ::onnxruntime::InferenceSession*>(sess);
  std::pair<Status, const InputDefList*> p = is_input ? session->GetModelInputs() : session->GetModelOutputs();
  if (!p.first.IsOK())
    return ToOrtStatus(p.first);
  if (p.second == nullptr)
    return OrtCreateStatus(ORT_FAIL, "internal error");
  const InputDefList& defs = *p.second;
  if (index >= defs.size())
    return OrtCreateStatus(ORT_FAIL, "index out of range");
  *output = StrDup(defs[index]->Name(), allocator);
  return nullptr;
}

ORT_API_STATUS_IMPL(OrtIsTensor, _In_ const OrtValue* value, int* out) {
  auto v = reinterpret_cast<const ::OrtValue*>(value);
  *out = v->IsTensor() ? 1 : 0;
  return nullptr;
}

ORT_API_STATUS_IMPL(OrtAllocatorAlloc, _Inout_ OrtAllocator* ptr, size_t size, _Out_ void** out) {
  API_IMPL_BEGIN
  *out = ptr->Alloc(ptr, size);
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtAllocatorFree, _Inout_ OrtAllocator* ptr, void* p) {
  API_IMPL_BEGIN
  ptr->Free(ptr, p);
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtAllocatorGetInfo, _In_ const OrtAllocator* ptr, _Out_ const struct OrtAllocatorInfo** out) {
  API_IMPL_BEGIN
  *out = ptr->Info(ptr);
  return nullptr;
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtSessionGetInputName, _In_ const OrtSession* sess, size_t index,
                    _Inout_ OrtAllocator* allocator, _Out_ char** output) {
  API_IMPL_BEGIN
  return GetInputOutputNameImpl(sess, index, allocator, true, output);
  API_IMPL_END
}

ORT_API_STATUS_IMPL(OrtSessionGetOutputName, _In_ const OrtSession* sess, size_t index,
                    _Inout_ OrtAllocator* allocator, _Out_ char** output) {
  API_IMPL_BEGIN
  return GetInputOutputNameImpl(sess, index, allocator, false, output);
  API_IMPL_END
}

///////////////////////////////////////////////////////////////////////////
// Code to handle non-tensor types
// OrtGetValueCount
// OrtGetVaue
// OrtCreateValue
///////////////////////////////////////////////////////////////////////////
const int NUM_MAP_INDICES = 2;

////////////////////
// OrtGetValueCount
template <typename T>
OrtStatus* OrtGetNumSequenceElements(const OrtValue* p_ml_value, size_t* out) {
  auto& data = p_ml_value->Get<T>();
  *out = data.size();
  return nullptr;
}

static OrtStatus* OrtGetValueCountImpl(const OrtValue* value, size_t* out) {
  ONNXType value_type;
  if (auto status = OrtGetValueType(value, &value_type))
    return status;
  if (value_type == ONNX_TYPE_MAP) {
    *out = NUM_MAP_INDICES;
    return nullptr;
  }
  if (value_type == ONNX_TYPE_SEQUENCE) {
    auto v = reinterpret_cast<const OrtValue*>(value);
    auto type = v->Type();
    // Note: keep these in sync with the registered types in data_types.h
    if (type == DataTypeImpl::GetType<VectorString>()) {
      return OrtGetNumSequenceElements<VectorString>(v, out);
    }
    if (type == DataTypeImpl::GetType<VectorInt64>()) {
      return OrtGetNumSequenceElements<VectorInt64>(v, out);
    } else if (type == DataTypeImpl::GetType<VectorFloat>()) {
      return OrtGetNumSequenceElements<VectorFloat>(v, out);
    } else if (type == DataTypeImpl::GetType<VectorDouble>()) {
      return OrtGetNumSequenceElements<VectorDouble>(v, out);
    } else if (type == DataTypeImpl::GetType<VectorMapStringToFloat>()) {
      return OrtGetNumSequenceElements<VectorMapStringToFloat>(v, out);
    } else if (type == DataTypeImpl::GetType<VectorMapInt64ToFloat>()) {
      return OrtGetNumSequenceElements<VectorMapInt64ToFloat>(v, out);
    } else {
      return OrtCreateStatus(ORT_FAIL, "Input is not of one of the supported sequence types.");
    }
  } else {
    return OrtCreateStatus(ORT_FAIL, "Input is not of type sequence or map.");
  }
}

ORT_API_STATUS_IMPL(OrtGetValueCount, const OrtValue* value, size_t* out) {
  API_IMPL_BEGIN
  return OrtGetValueCountImpl(value, out);
  API_IMPL_END
}

///////////////////
// OrtGetValue
template <typename T>
static OrtStatus* OrtGetValueImplSeqOfMap(const OrtValue* p_ml_value, int index, OrtValue** out) {
  using TKey = typename T::value_type::key_type;
  using TVal = typename T::value_type::mapped_type;
  using MapType = std::map<TKey, TVal>;
  auto& data_vec = p_ml_value->Get<T>();
  auto& data_elem = data_vec.at(index);
  auto copy_data_elem = std::make_unique<MapType>(data_elem);
  auto value = std::make_unique<OrtValue>();
  value->Init(copy_data_elem.release(),
              DataTypeImpl::GetType<MapType>(),
              DataTypeImpl::GetType<MapType>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
}

template <typename T>
ONNXTensorElementDataType GetONNXTensorElementDataType() {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
}

template <>
ONNXTensorElementDataType GetONNXTensorElementDataType<std::string>() {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;
}

template <>
ONNXTensorElementDataType GetONNXTensorElementDataType<float>() {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
}

template <>
ONNXTensorElementDataType GetONNXTensorElementDataType<double>() {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;
}

template <>
ONNXTensorElementDataType GetONNXTensorElementDataType<int64_t>() {
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
}

template <typename T>
OrtStatus* PopulateTensorWithData(OrtValue* oval, const T* data_elem, size_t num_elems) {
  void* raw_data = nullptr;
  auto st = OrtGetTensorMutableData(oval, &raw_data);
  if (st) {
    return st;
  }
  memcpy(raw_data, data_elem, sizeof(T) * num_elems);
  return nullptr;
}

template <>
OrtStatus* PopulateTensorWithData<std::string>(OrtValue* oval, const std::string* data_elem,
                                               size_t num_elems) {
  auto v = reinterpret_cast<OrtValue*>(oval);
  auto tensor = v->GetMutable<Tensor>();
  auto* dst = tensor->MutableData<std::string>();
  auto len = static_cast<size_t>(tensor->Shape().Size());
  if (num_elems < len) {
    return OrtCreateStatus(ORT_INVALID_ARGUMENT, "input array is too short");
  }
  for (size_t i = 0; i < len; ++i) {
    dst[i] = data_elem[i];
  }
  return nullptr;
}

template <typename T>
OrtStatus* OrtGetValueImplSeqOfPrimitives(const OrtValue* p_ml_value, int index, OrtAllocator* allocator,
                                          OrtValue** out) {
  using ElemType = typename T::value_type;
  auto& data = p_ml_value->Get<T>();
  auto& data_elem = data.at(index);
  std::vector<int64_t> dims = {1};
  OrtStatus* st = OrtCreateTensorAsOrtValue(allocator, dims.data(), dims.size(),
                                            GetONNXTensorElementDataType<ElemType>(), out);
  return st ? st : PopulateTensorWithData<ElemType>(*out, &data_elem, 1);
}

static OrtStatus* OrtGetValueImplSeq(const OrtValue* value, int index, OrtAllocator* allocator,
                                     OrtValue** out) {
  auto p_ml_value = reinterpret_cast<const OrtValue*>(value);
  auto type = p_ml_value->Type();
  // Note: keep these in sync with the registered types in data_types.h
  if (type == DataTypeImpl::GetType<VectorString>()) {
    return OrtGetValueImplSeqOfPrimitives<VectorString>(p_ml_value, index, allocator, out);
  }
  if (type == DataTypeImpl::GetType<VectorInt64>()) {
    return OrtGetValueImplSeqOfPrimitives<VectorInt64>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<VectorFloat>()) {
    return OrtGetValueImplSeqOfPrimitives<VectorFloat>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<VectorDouble>()) {
    return OrtGetValueImplSeqOfPrimitives<VectorDouble>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<VectorMapStringToFloat>()) {
    return OrtGetValueImplSeqOfMap<VectorMapStringToFloat>(p_ml_value, index, out);
  } else if (type == DataTypeImpl::GetType<VectorMapInt64ToFloat>()) {
    return OrtGetValueImplSeqOfMap<VectorMapInt64ToFloat>(p_ml_value, index, out);
  } else {
    return OrtCreateStatus(ORT_FAIL, "Input is not of one of the supported sequence types.");
  }
}

template <typename T>
static OrtStatus* OrtGetValueImplMapHelper(const OrtValue* p_ml_value, int index, OrtAllocator* allocator,
                                           OrtValue** out) {
  using TKey = typename T::key_type;
  using TVal = typename T::mapped_type;
  auto& data = p_ml_value->Get<T>();
  int64_t num_kv_pairs = data.size();
  switch (index) {
    case 0: {  // user is requesting keys
      std::vector<TKey> vec;
      vec.reserve(num_kv_pairs);
      for (const auto& kv : data) {
        vec.push_back(kv.first);
      }
      std::vector<int64_t> dims{num_kv_pairs};
      OrtStatus* st = OrtCreateTensorAsOrtValue(allocator, dims.data(), dims.size(),
                                                GetONNXTensorElementDataType<TKey>(), out);
      return st ? st : PopulateTensorWithData<TKey>(*out, vec.data(), num_kv_pairs);
    }
    case 1: {  // user is requesting values
      std::vector<TVal> vec;
      vec.reserve(num_kv_pairs);
      for (const auto& kv : data) {
        vec.push_back(kv.second);
      }
      std::vector<int64_t> dims{num_kv_pairs};
      OrtStatus* st = OrtCreateTensorAsOrtValue(allocator, dims.data(), dims.size(),
                                                GetONNXTensorElementDataType<TVal>(), out);
      return st ? st : PopulateTensorWithData<TVal>(*out, vec.data(), num_kv_pairs);
    }
    default:
      return OrtCreateStatus(ORT_FAIL, "Invalid index requested for map type.");
  }
}

static OrtStatus* OrtGetValueImplMap(const OrtValue* value, int index, OrtAllocator* allocator,
                                     OrtValue** out) {
  auto p_ml_value = reinterpret_cast<const OrtValue*>(value);
  auto type = p_ml_value->Type();
  // Note: keep these in sync with the registered types in data_types.h
  if (type == DataTypeImpl::GetType<MapStringToString>()) {
    return OrtGetValueImplMapHelper<MapStringToString>(p_ml_value, index, allocator, out);
  }
  if (type == DataTypeImpl::GetType<MapStringToInt64>()) {
    return OrtGetValueImplMapHelper<MapStringToInt64>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapStringToFloat>()) {
    return OrtGetValueImplMapHelper<MapStringToFloat>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapStringToDouble>()) {
    return OrtGetValueImplMapHelper<MapStringToDouble>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapInt64ToString>()) {
    return OrtGetValueImplMapHelper<MapInt64ToString>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapInt64ToInt64>()) {
    return OrtGetValueImplMapHelper<MapInt64ToInt64>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapInt64ToFloat>()) {
    return OrtGetValueImplMapHelper<MapInt64ToFloat>(p_ml_value, index, allocator, out);
  } else if (type == DataTypeImpl::GetType<MapInt64ToDouble>()) {
    return OrtGetValueImplMapHelper<MapInt64ToDouble>(p_ml_value, index, allocator, out);
  } else {
    return OrtCreateStatus(ORT_FAIL, "Input is not of one of the supported map types.");
  }
}

static OrtStatus* OrtGetValueImpl(const OrtValue* value, int index, OrtAllocator* allocator,
                                  OrtValue** out) {
  ONNXType value_type;
  if (auto status = OrtGetValueType(value, &value_type))
    return status;
  if (value_type == ONNX_TYPE_MAP) {
    return OrtGetValueImplMap(value, index, allocator, out);
  }
  if (value_type == ONNX_TYPE_SEQUENCE) {
    return OrtGetValueImplSeq(value, index, allocator, out);
  } else {
    return OrtCreateStatus(ORT_FAIL, "Input is not of type sequence or map.");
  }
}

ORT_API_STATUS_IMPL(OrtGetValue, const OrtValue* value, int index, OrtAllocator* allocator,
                    OrtValue** out) {
  API_IMPL_BEGIN
  return OrtGetValueImpl(value, index, allocator, out);
  API_IMPL_END
}

///////////////////
// OrtCreateValue
template <typename T>
static OrtStatus* OrtCreateValueImplSeqHelperMap(OrtValue** const in, size_t num_values, OrtValue** out) {
  using SeqType = std::vector<T>;
  auto vec_ptr = std::make_unique<SeqType>();
  vec_ptr->reserve(num_values);
  for (size_t idx = 0; idx < num_values; ++idx) {
    auto& m = reinterpret_cast<const OrtValue*>(in[idx])->Get<T>();
    vec_ptr->push_back(m);
  }
  // create OrtValue with this vector
  auto value = std::make_unique<OrtValue>();
  value->Init(vec_ptr.release(),
              DataTypeImpl::GetType<SeqType>(),
              DataTypeImpl::GetType<SeqType>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
}

template <typename T>
static OrtStatus* OrtCreateValueImplSeqHelper(OrtValue** in, size_t num_values, OrtValue** out) {
  using SeqType = std::vector<T>;
  auto vec_ptr = std::make_unique<SeqType>();
  vec_ptr->reserve(num_values);
  for (size_t idx = 0; idx < num_values; ++idx) {
    auto& tensor = reinterpret_cast<const OrtValue*>(in[idx])->Get<Tensor>();
    auto data = tensor.Data<T>();
    if (!data) {
      return OrtCreateStatus(ORT_FAIL, "Encountered nullptr.");
    }
    vec_ptr->push_back(*data);
  }
  // create OrtValue with this vector
  auto value = std::make_unique<OrtValue>();
  value->Init(vec_ptr.release(),
              DataTypeImpl::GetType<SeqType>(),
              DataTypeImpl::GetType<SeqType>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
}

static OrtStatus* OrtCreateValueImplSeq(OrtValue** in, size_t num_values, OrtValue** out) {
  // We only support limited sequence types. For the sake of simplicity the type of the first
  // OrtValue* in OrtValue** will determine the type of the vector used to create the output OrtValue
  // this type should be either a tensor of limited types or map of limited types
  const OrtValue* ovfirst = in[0];
  ONNXType first_value_type;
  if (auto status = OrtGetValueType(ovfirst, &first_value_type))
    return status;
  // in onnxruntime type registrations we can support only a fixed vector types
  // this check ensures that the input conforms to that
  if (!(first_value_type == ONNX_TYPE_TENSOR || first_value_type == ONNX_TYPE_MAP)) {
    return OrtCreateStatus(ORT_FAIL, "Each element of the sequence should be either tensor or map.");
  }
  // check if all OrtValues in the input array are of the same type
  // this is because even though the ONNX spec and this API spec supports heterogenous sequences,
  // only a fixed types are registered in onnxruntime
  for (size_t i = 0; i < num_values; ++i) {
    const OrtValue* ov = in[i];
    ONNXType ov_type;
    if (auto status = OrtGetValueType(ov, &ov_type))
      return status;
    if (ov_type != first_value_type) {
      return OrtCreateStatus(ORT_FAIL,
                             "At least one element in the sequence is of a type different from others.");
    }
  }

  // finally create the output vector/MLValue
  auto first_mlvalue = reinterpret_cast<const OrtValue*>(ovfirst);
  if (first_value_type == ONNX_TYPE_TENSOR) {
    auto vec_type = first_mlvalue->Get<Tensor>().DataType();
    if (vec_type == DataTypeImpl::GetType<std::string>()) {
      return OrtCreateValueImplSeqHelper<std::string>(in, num_values, out);
    }
    if (vec_type == DataTypeImpl::GetType<int64_t>()) {
      return OrtCreateValueImplSeqHelper<int64_t>(in, num_values, out);
    } else if (vec_type == DataTypeImpl::GetType<float>()) {
      return OrtCreateValueImplSeqHelper<float>(in, num_values, out);
    } else if (vec_type == DataTypeImpl::GetType<double>()) {
      return OrtCreateValueImplSeqHelper<double>(in, num_values, out);
    } else {
      return OrtCreateStatus(ORT_FAIL, "Type not supported.");
    }
  } else if (first_value_type == ONNX_TYPE_MAP) {
    auto map_type = first_mlvalue->Type();
    if (map_type == DataTypeImpl::GetType<MapStringToFloat>()) {
      return OrtCreateValueImplSeqHelperMap<MapStringToFloat>(in, num_values, out);
    }
    if (map_type == DataTypeImpl::GetType<MapInt64ToFloat>()) {
      return OrtCreateValueImplSeqHelperMap<MapInt64ToFloat>(in, num_values, out);
    } else {
      return OrtCreateStatus(ORT_FAIL, "Input is not of one of the supported map types.");
    }
  } else {
    return OrtCreateStatus(ORT_FAIL, "Unsupported input type");
  }
}

template <typename KeyType, typename ValueType>
static OrtStatus* OrtCreateMapMLValue(const Tensor& key_tensor, const Tensor& value_tensor,
                                      OrtValue** out) {
  using MapType = std::map<KeyType, ValueType>;
  auto map_ptr = std::make_unique<MapType>();
  // iterate through the key and value tensors and populate map
  auto key_data = key_tensor.Data<KeyType>();
  auto value_data = value_tensor.Data<ValueType>();
  size_t num_kv_pairs = key_tensor.Shape().Size();
  for (size_t n = 0; n < num_kv_pairs; ++n, ++key_data, ++value_data) {
    map_ptr->insert({*key_data, *value_data});
  }
  // create ort_value with this map
  auto value = std::make_unique<OrtValue>();
  value->Init(map_ptr.release(),
              DataTypeImpl::GetType<MapType>(),
              DataTypeImpl::GetType<MapType>()->GetDeleteFunc());
  *out = value.release();
  return nullptr;
}

template <typename KeyType>
static OrtStatus* OrtCreateValueImplMapHelper(const Tensor& key_tensor, const Tensor& value_tensor,
                                              OrtValue** out) {
  auto value_type = value_tensor.DataType();
  if (value_type == DataTypeImpl::GetType<std::string>()) {
    return OrtCreateMapMLValue<KeyType, std::string>(key_tensor, value_tensor, out);
  }
  if (value_type == DataTypeImpl::GetType<int64_t>()) {
    return OrtCreateMapMLValue<KeyType, int64_t>(key_tensor, value_tensor, out);
  } else if (value_type == DataTypeImpl::GetType<float>()) {
    return OrtCreateMapMLValue<KeyType, float>(key_tensor, value_tensor, out);
  } else if (value_type == DataTypeImpl::GetType<double>()) {
    return OrtCreateMapMLValue<KeyType, double>(key_tensor, value_tensor, out);
  } else {
    return OrtCreateStatus(ORT_FAIL, "Value type is not supported yet.");
  }
}

static OrtStatus* OrtCreateValueImplMap(OrtValue** in, size_t num_values, OrtValue** out) {
  if (num_values != NUM_MAP_INDICES) {
    return OrtCreateStatus(ORT_FAIL, "For map type num_values MUST be 2");
  }

  const OrtValue* ort_keys = in[0];
  auto p_key_ml_value = reinterpret_cast<const OrtValue*>(ort_keys);
  auto& key_tensor = p_key_ml_value->Get<Tensor>();
  auto key_type = key_tensor.DataType();

  const OrtValue* ort_values = in[1];
  auto p_value_ml_value = reinterpret_cast<const OrtValue*>(ort_values);
  auto& value_tensor = p_value_ml_value->Get<Tensor>();

  // as per data_types.h, we only support maps of primitive data types.
  if (key_tensor.Shape().NumDimensions() > 1 || value_tensor.Shape().NumDimensions() > 1) {
    return OrtCreateStatus(ORT_FAIL, "Either the key tensor or the value tensor has NumDimensions > 1");
  }

  // since maps are represented by key and value tensors, their sizes have to be the same.
  if (key_tensor.Shape().Size() != value_tensor.Shape().Size()) {
    return OrtCreateStatus(ORT_FAIL, "Key and value tensors have unequal number of elements.");
  }

  if (key_type == DataTypeImpl::GetType<std::string>()) {
    return OrtCreateValueImplMapHelper<std::string>(key_tensor, value_tensor, out);
  }
  if (key_type == DataTypeImpl::GetType<int64_t>()) {
    return OrtCreateValueImplMapHelper<int64_t>(key_tensor, value_tensor, out);
  }
  return OrtCreateStatus(ORT_FAIL, "Key type is not supported yet.");
}

static OrtStatus* OrtCreateValueImpl(OrtValue** in, size_t num_values, enum ONNXType value_type, OrtValue** out) {
  if (num_values <= 0) {
    return OrtCreateStatus(ORT_FAIL, "Number of values should be at least 1.");
  }
  if (value_type == ONNX_TYPE_MAP) {
    return OrtCreateValueImplMap(in, num_values, out);
  }
  if (value_type == ONNX_TYPE_SEQUENCE) {
    return OrtCreateValueImplSeq(in, num_values, out);
  }
  return OrtCreateStatus(ORT_FAIL, "Input is not of type sequence or map.");
}

ORT_API_STATUS_IMPL(OrtCreateValue, OrtValue** in, size_t num_values, enum ONNXType value_type, OrtValue** out) {
  API_IMPL_BEGIN
  return OrtCreateValueImpl(in, num_values, value_type, out);
  API_IMPL_END
}

// End support for non-tensor types

DEFINE_RELEASE_ORT_OBJECT_FUNCTION(Env, OrtEnv)
DEFINE_RELEASE_ORT_OBJECT_FUNCTION(Value, OrtValue)
DEFINE_RELEASE_ORT_OBJECT_FUNCTION(RunOptions, OrtRunOptions)
DEFINE_RELEASE_ORT_OBJECT_FUNCTION(Session, ::onnxruntime::InferenceSession)
