/* Copyright 2019 The OpenXLA Authors.

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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#if GOOGLE_CUDA
#include "third_party/gpus/cuda/include/cuda.h"  // IWYU pragma: keep
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "third_party/gpus/cuda/include/driver_types.h"
#define PLATFORM "CUDA"
#elif TENSORFLOW_USE_ROCM
#include "rocm/include/hip/hip_runtime.h"
#define PLATFORM "ROCM"
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "xla/client/lib/constants.h"
#include "xla/client/xla_builder.h"
#include "xla/ffi/ffi.h"
#include "xla/ffi/ffi_api.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/custom_call_status.h"
#include "xla/service/custom_call_target_registry.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/scratch_allocator.h"
#include "xla/stream_executor/stream.h"
#include "xla/test_helpers.h"
#include "xla/tests/client_library_test_base.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/statusor.h"

#if GOOGLE_CUDA
#define gpuSuccess cudaSuccess
#define gpuMemcpyAsync cudaMemcpyAsync
#define gpuMemcpyDeviceToDevice cudaMemcpyDeviceToDevice
#define gpuMemcpy cudaMemcpy
#define gpuMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define gpuMemcpyHostToDevice cudaMemcpyHostToDevice
#elif TENSORFLOW_USE_ROCM
#define gpuSuccess hipSuccess
#define gpuMemcpyAsync hipMemcpyAsync
#define gpuMemcpyDeviceToDevice hipMemcpyDeviceToDevice
#define gpuMemcpy hipMemcpy
#define gpuMemcpyDeviceToHost hipMemcpyDeviceToHost
#define gpuMemcpyHostToDevice hipMemcpyHostToDevice
#endif

namespace xla {
namespace {

class CustomCallTest : public ClientLibraryTestBase {};

bool is_invoked_called = false;
void Callback_IsInvoked(se::gpu::GpuStreamHandle /*stream*/, void** /*buffers*/,
                        const char* /*opaque*/, size_t /*opaque_len*/) {
  is_invoked_called = true;
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_IsInvoked, PLATFORM);

TEST_F(CustomCallTest, IsInvoked) {
  XlaBuilder b(TestName());
  CustomCall(&b, "Callback_IsInvoked", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}),
             /*opaque=*/"");
  EXPECT_FALSE(is_invoked_called);
  TF_ASSERT_OK(Execute(&b, {}).status());
  EXPECT_TRUE(is_invoked_called);
}

TEST_F(CustomCallTest, UnknownTarget) {
  XlaBuilder b(TestName());
  CustomCall(&b, "UnknownTarget", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}),
             /*opaque=*/"");
  ASSERT_FALSE(Execute(&b, {}).ok());
}
void Callback_Memcpy(se::gpu::GpuStreamHandle stream, void** buffers,
                     const char* /*opaque*/, size_t /*opaque_len*/) {
  void* src = buffers[0];
  void* dst = buffers[1];
  auto err = gpuMemcpyAsync(dst, src, /*count=*/sizeof(float) * 128,
                            gpuMemcpyDeviceToDevice, stream);
  ASSERT_EQ(err, gpuSuccess);
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_Memcpy, PLATFORM);
TEST_F(CustomCallTest, Memcpy) {
  XlaBuilder b(TestName());
  CustomCall(&b, "Callback_Memcpy",
             /*operands=*/{Broadcast(ConstantR0WithType(&b, F32, 42.0), {128})},
             ShapeUtil::MakeShape(F32, {128}), /*opaque=*/"");
  TF_ASSERT_OK_AND_ASSIGN(auto result, ExecuteAndTransfer(&b, {}));
  EXPECT_THAT(result.data<float>(), ::testing::Each(42));
}

// Check that opaque handles nulls within the string.
std::string& kExpectedOpaque = *new std::string("abc\0def", 7);
void Callback_Opaque(se::gpu::GpuStreamHandle /*stream*/, void** /*buffers*/,
                     const char* opaque, size_t opaque_len) {
  std::string opaque_str(opaque, opaque_len);
  ASSERT_EQ(opaque_str, kExpectedOpaque);
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_Opaque, PLATFORM);
TEST_F(CustomCallTest, Opaque) {
  XlaBuilder b(TestName());
  CustomCall(&b, "Callback_Opaque", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}), kExpectedOpaque);
  TF_ASSERT_OK(Execute(&b, {}).status());
}

void Callback_SubBuffers(se::gpu::GpuStreamHandle stream, void** buffers,
                         const char* /*opaque*/, size_t /*opaque_len*/) {
  // `buffers` is a flat array containing device pointers to the following.
  //
  //  0:  param 0 at tuple index {0}, shape f32[128]
  //  1:  param 0 at tuple index {1}, shape f32[256]
  //  2:  param 1 at tuple index {0}, shape f32[1024]
  //  3:  param 1 at tuple index {1}, shape f32[8]
  //  4:  result at tuple index {0}, shape f32[8]
  //  5:  result at tuple index {1, 0}, shape f32[128]
  //  6:  result at tuple index {1, 1}, shape f32[256]
  //  7:  result at tuple index {2}, shape f32[1024]
  //

  // Set output leaf buffers, copying data from the corresponding same-sized
  // inputs.
  auto err = gpuMemcpyAsync(buffers[4], buffers[3], 8 * sizeof(float),
                            gpuMemcpyDeviceToDevice, stream);
  ASSERT_EQ(err, gpuSuccess);
  err = gpuMemcpyAsync(buffers[5], buffers[0], 128 * sizeof(float),
                       gpuMemcpyDeviceToDevice, stream);
  ASSERT_EQ(err, gpuSuccess);
  err = gpuMemcpyAsync(buffers[6], buffers[1], 256 * sizeof(float),
                       gpuMemcpyDeviceToDevice, stream);
  ASSERT_EQ(err, gpuSuccess);
  err = gpuMemcpyAsync(buffers[7], buffers[2], 1024 * sizeof(float),
                       gpuMemcpyDeviceToDevice, stream);
  ASSERT_EQ(err, gpuSuccess);
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_SubBuffers, PLATFORM);
TEST_F(CustomCallTest, SubBuffers) {
  XlaBuilder b(TestName());
  CustomCall(&b, "Callback_SubBuffers", /*operands=*/
             {
                 Tuple(&b,
                       {
                           Broadcast(ConstantR0WithType(&b, F32, 1), {128}),
                           Broadcast(ConstantR0WithType(&b, F32, 2), {256}),
                       }),
                 Tuple(&b,
                       {
                           Broadcast(ConstantR0WithType(&b, F32, 3), {1024}),
                           Broadcast(ConstantR0WithType(&b, F32, 4), {8}),
                       }),
             },
             ShapeUtil::MakeTupleShape({
                 ShapeUtil::MakeShape(F32, {8}),
                 ShapeUtil::MakeTupleShape({
                     ShapeUtil::MakeShape(F32, {128}),
                     ShapeUtil::MakeShape(F32, {256}),
                 }),
                 ShapeUtil::MakeShape(F32, {1024}),
             }),
             /*opaque=*/"");
  TF_ASSERT_OK_AND_ASSIGN(auto result, ExecuteAndTransfer(&b, {}));
  EXPECT_THAT(result.data<float>({0}), ::testing::Each(4));
  EXPECT_THAT(result.data<float>({1, 0}), ::testing::Each(1));
  EXPECT_THAT(result.data<float>({1, 1}), ::testing::Each(2));
  EXPECT_THAT(result.data<float>({2}), ::testing::Each(3));
}

// The test case for custom call with tokens encodes the arguments and result
// type using a string with A(=Array), T(=Token) and {} for Tuples. It also
// encodes the check that the callback has to do in terms of a string of A and T
// where all the As need to be non-null and all the Ts need to be null. This is
// passed to the custom call as its opaque data.
//
// As an example, "ATTA" for an input encodes 4 inputs to custom call,
// "{A{A}T}" for output encodes a custom call with return type containing a
// single tuple, with another tuple as the 2nd element. For outputs, it is
// either a single element or a tuple. Note, no error checking is performed.

struct TokenTestCase {
  std::string input;
  std::string output;
  std::string opaque;
};

std::ostream& operator<<(std::ostream& s, const TokenTestCase& tc) {
  s << tc.input << "x" << tc.output << "x" << tc.opaque;
  return s;
}

void Callback_Tokens(se::gpu::GpuStreamHandle stream, void** buffers,
                     const char* opaque, size_t opaque_len) {
  for (int i = 0; i < opaque_len; ++i) {
    char c = opaque[i];
    ASSERT_TRUE(c == 'A' || c == 'T');
    if (c == 'A') {
      ASSERT_NE(buffers[i], nullptr);
    } else {
      ASSERT_EQ(buffers[i], nullptr);
    }
  }
}

XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_Tokens, PLATFORM);

std::vector<TokenTestCase> GetTokenTestCases() {
  return {{"{AT}{AT}", "{A{AT}A}", "ATATAATA"},  // tokens in input and output
          {"{A}", "T", "AT"},                    // single token as output
          {"{{T}}", "A", "TA"},                  // single token as input
          {"AA", "{TA}", "AATA"},
          {"TA{TA{TA}}", "{AA}", "TATATAAA"}};
}

class CustomCallTokensTest
    : public ::testing::WithParamInterface<TokenTestCase>,
      public ClientLibraryTestBase {
 public:
  static std::vector<XlaOp> BuildInputs(XlaBuilder& b,
                                        std::istringstream& str) {
    std::vector<XlaOp> values;
    while (!str.eof()) {
      int ch = str.get();
      if (ch == 'A') {
        values.push_back(Broadcast(ConstantR0WithType(&b, F32, 1), {128}));
      } else if (ch == 'T') {
        values.push_back(CreateToken(&b));
      } else if (ch == '{') {
        // build a tuple of values. This will eat the } as well.
        std::vector<XlaOp> tuple_elements = BuildInputs(b, str);
        values.push_back(Tuple(&b, tuple_elements));
      } else if (ch == '}') {
        break;
      }
    }
    return values;
  }

  static std::vector<Shape> BuildOutputType(std::istringstream& str) {
    std::vector<Shape> shapes;
    while (!str.eof()) {
      int ch = str.get();
      if (ch == 'A') {
        shapes.push_back(ShapeUtil::MakeShape(F32, {8}));
      } else if (ch == 'T') {
        shapes.push_back(ShapeUtil::MakeTokenShape());
      } else if (ch == '{') {
        // build a tuple shape. This will eat the } as well.
        std::vector<Shape> tuple_elements = BuildOutputType(str);
        shapes.push_back(ShapeUtil::MakeTupleShape(tuple_elements));
      } else if (ch == '}') {
        break;
      }
    }
    return shapes;
  }
};

TEST_P(CustomCallTokensTest, TokensTest) {
  const TokenTestCase& tc = GetParam();

  XlaBuilder b("CustomCallTokens");

  std::istringstream input(tc.input);
  std::istringstream output(tc.output);
  std::vector<XlaOp> call_inputs = BuildInputs(b, input);
  std::vector<Shape> call_output = BuildOutputType(output);
  ASSERT_EQ(call_output.size(), 1);

  CustomCall(&b, "Callback_Tokens", call_inputs, call_output.front(),
             tc.opaque);
  TF_ASSERT_OK(Execute(&b, {}).status());
}

INSTANTIATE_TEST_CASE_P(CustomCallTokens, CustomCallTokensTest,
                        ::testing::ValuesIn(GetTokenTestCases()));

void Callback_WithStatusSucceeded(se::gpu::GpuStreamHandle /*stream*/,
                                  void** /*buffers*/, const char* /*opaque*/,
                                  size_t /*opaque_len*/,
                                  XlaCustomCallStatus* status) {
  XlaCustomCallStatusSetSuccess(status);
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_WithStatusSucceeded, PLATFORM);

TEST_F(CustomCallTest, WithStatusSucceeded) {
  XlaBuilder b(TestName());
  CustomCall(
      &b, "Callback_WithStatusSucceeded", /*operands=*/{},
      ShapeUtil::MakeShape(F32, {}), /*opaque=*/"",
      /*has_side_effect=*/false,
      /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
      /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
      /*api_version=*/CustomCallApiVersion::API_VERSION_STATUS_RETURNING);
  TF_ASSERT_OK(Execute(&b, {}).status());
}

void Callback_WithStatusFailed(se::gpu::GpuStreamHandle /*stream*/,
                               void** /*buffers*/, const char* /*opaque*/,
                               size_t /*opaque_len*/,
                               XlaCustomCallStatus* status) {
  XlaCustomCallStatusSetFailure(status, "Failed", 6);
}
XLA_REGISTER_CUSTOM_CALL_TARGET(Callback_WithStatusFailed, PLATFORM);

TEST_F(CustomCallTest, WithStatusFailed) {
  XlaBuilder b(TestName());
  CustomCall(
      &b, "Callback_WithStatusFailed", /*operands=*/{},
      ShapeUtil::MakeShape(F32, {}), /*opaque=*/"",
      /*has_side_effect=*/false,
      /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
      /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
      /*api_version=*/CustomCallApiVersion::API_VERSION_STATUS_RETURNING);
  auto status = Execute(&b, {}).status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
  EXPECT_THAT(status.message(), ::testing::HasSubstr("Failed"));
}

//===----------------------------------------------------------------------===//
// XLA runtime custom calls provides type-safe custom call API
//===----------------------------------------------------------------------===//

static absl::Status AlwaysFail(ffi::BufferBase arg, int32_t value) {
  return absl::InternalError(absl::StrCat("Uh oh, wrong value: ", value));
}

XLA_FFI_DEFINE_HANDLER(kAlwaysFail, AlwaysFail,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // arg
                           .Attr<int32_t>("value")  // value
);
XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$always_fail",
                         PLATFORM, kAlwaysFail);

TEST_F(CustomCallTest, RuntimeCustomCallAlwaysFail) {
  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$always_fail", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}), /*opaque=*/"{value = 42 : i32}",
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  auto status = Execute(&b, {}).status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
  EXPECT_THAT(status.message(), ::testing::HasSubstr("Uh oh, wrong value: 42"));
}

static absl::Status Memcpy(se::Stream* stream, ffi::BufferBase src,
                           ffi::BufferBase dst) {
  return stream->MemcpyD2D(
      &dst.data, src.data,
      absl::c_accumulate(src.dimensions, 1.0, std::multiplies<int64_t>()) *
          sizeof(float));
}

XLA_FFI_DEFINE_HANDLER(kMemcpy, Memcpy,
                       ffi::Ffi::Bind()
                           .Ctx<se::Stream>()
                           .Arg<ffi::BufferBase>()  // src
                           .Arg<ffi::BufferBase>()  // dst
);
XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$memcpy", PLATFORM,
                         kMemcpy);

TEST_F(CustomCallTest, ExportedFfiMemcpy) {
  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$memcpy",
             /*operands=*/{Broadcast(ConstantR0WithType(&b, F32, 42.0), {128})},
             ShapeUtil::MakeShape(F32, {128}), /*opaque=*/"",
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  TF_ASSERT_OK_AND_ASSIGN(auto result, ExecuteAndTransfer(&b, {}));
  EXPECT_THAT(result.data<float>(), ::testing::Each(42));
}

static absl::Status HandleUserPointer(ffi::BufferBase, const std::string* str) {
  return absl::InternalError(*str);
}

XLA_FFI_DEFINE_HANDLER(kHandleUserPointer, HandleUserPointer,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // buffer for result
                           .Attr<ffi::Pointer<std::string>>("message"));

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$user_data", PLATFORM,
                         kHandleUserPointer);

TEST_F(CustomCallTest, PassUserPointerWithAttrs) {
  std::string message = "User-defined message";
  auto ptr = reinterpret_cast<uintptr_t>(&message);

  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$user_data", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}),
             /*opaque=*/absl::StrFormat("{message = %d : i64}", ptr),
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  auto status = Execute(&b, {}).status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
  EXPECT_THAT(status.message(), ::testing::HasSubstr("User-defined message"));
}

bool is_ffi_invoked = false;
static absl::Status IsInvoked(ffi::BufferBase) {
  is_ffi_invoked = true;
  return absl::OkStatus();
}

XLA_FFI_DEFINE_HANDLER(
    kIsInvoked, IsInvoked,
    ffi::Ffi::Bind().Arg<ffi::BufferBase>());  // Buffer for result (unused).

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$isinvoked", PLATFORM,
                         kIsInvoked);

TEST_F(CustomCallTest, ExportedFfiIsInvoked) {
  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$isinvoked", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}), /*opaque=*/"",
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  TF_ASSERT_OK_AND_ASSIGN(auto result, ExecuteAndTransfer(&b, {}));
  EXPECT_TRUE(is_ffi_invoked);
}

TEST_F(CustomCallTest, ExportedFfiUnknownTarget) {
  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$unknown_target", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}), /*opaque=*/"",
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  auto status = Execute(&b, {}).status();
  EXPECT_EQ(status.code(), absl::StatusCode::kUnimplemented);
  EXPECT_THAT(status.message(),
              ::testing::HasSubstr("No registered implementation"));
}

// Memcpy and SubBuffers tests are already ported in
// fusions/address_computation_fusion_test.cc

// Reusing kExpectedOpaque from the original test.
static absl::Status Opaque(ffi::BufferBase, const std::string* str) {
  std::string opaque(*str);
  if (opaque != kExpectedOpaque)
    return absl::InternalError(absl::StrFormat(
        "Opaque string does not match. Expected `%s` but got `%s`",
        kExpectedOpaque, opaque));
  return absl::OkStatus();
}

XLA_FFI_DEFINE_HANDLER(kOpaque, Opaque,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // Dummy result buffer.
                           .Attr<ffi::Pointer<std::string>>("opaque"));

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$opaque", PLATFORM,
                         kOpaque);

TEST_F(CustomCallTest, ExportedFfiOpaque) {
  XlaBuilder b(TestName());
  const std::string opaque = absl::StrFormat(
      "{opaque = %d : i64}", reinterpret_cast<uintptr_t>(&kExpectedOpaque));
  CustomCall(&b, "__xla_test$$opaque", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}),
             /*opaque=*/opaque,
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  TF_ASSERT_OK(Execute(&b, {}).status());
}

static absl::Status TokensChecker(std::vector<ffi::BufferBase> inputs,
                                  const std::string* opaque) {
  // TODO(penporn): Actually check the inputs when FFI handlers support tokens.
  return absl::OkStatus();
}

static absl::Status Tokens1Input(ffi::BufferBase input1, ffi::BufferBase,
                                 const std::string* opaque) {
  return TokensChecker({input1}, opaque);
}

static absl::Status Tokens2Inputs(ffi::BufferBase input1,
                                  ffi::BufferBase input2, ffi::BufferBase,
                                  const std::string* opaque) {
  return TokensChecker({input1, input2}, opaque);
}

static absl::Status Tokens3Inputs(ffi::BufferBase input1,
                                  ffi::BufferBase input2,
                                  ffi::BufferBase input3, ffi::BufferBase,
                                  const std::string* opaque) {
  return TokensChecker({input1, input2, input3}, opaque);
}

XLA_FFI_DEFINE_HANDLER(kTokens1Input, Tokens1Input,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // 1 input buffer.
                           .Arg<ffi::BufferBase>()  // Output buffer.
                           .Attr<ffi::Pointer<std::string>>("opaque"));

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$tokens_1input",
                         PLATFORM, kTokens1Input);

XLA_FFI_DEFINE_HANDLER(kTokens2Inputs, Tokens2Inputs,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // 1st input buffer.
                           .Arg<ffi::BufferBase>()  // 2nd input buffer.
                           .Arg<ffi::BufferBase>()  // Output buffer.
                           .Attr<ffi::Pointer<std::string>>("opaque"));

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$tokens_2inputs",
                         PLATFORM, kTokens2Inputs);

XLA_FFI_DEFINE_HANDLER(kTokens3Inputs, Tokens3Inputs,
                       ffi::Ffi::Bind()
                           .Arg<ffi::BufferBase>()  // 1st input buffer.
                           .Arg<ffi::BufferBase>()  // 2nd input buffer.
                           .Arg<ffi::BufferBase>()  // 3rd input buffer.
                           .Arg<ffi::BufferBase>()  // Output buffer.
                           .Attr<ffi::Pointer<std::string>>("opaque"));

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$tokens_3inputs",
                         PLATFORM, kTokens3Inputs);

TEST_P(CustomCallTokensTest, ExportedFfiTokensTest) {
  const TokenTestCase& tc = GetParam();
  XlaBuilder b(TestName());
  std::istringstream input(tc.input);
  std::istringstream output(tc.output);
  std::vector<XlaOp> call_inputs = BuildInputs(b, input);
  std::vector<Shape> call_output = BuildOutputType(output);
  ASSERT_GE(call_inputs.size(), 1);
  ASSERT_LE(call_inputs.size(), 3);
  ASSERT_EQ(call_output.size(), 1);

  const std::string custom_call_name =
      absl::StrFormat("__xla_test$$tokens_%dinput%s", call_inputs.size(),
                      call_inputs.size() == 1 ? "" : "s");
  const std::string opaque = absl::StrFormat(
      "{opaque = %d : i64}", reinterpret_cast<uintptr_t>(&tc.opaque));
  CustomCall(&b, custom_call_name, /*operands=*/call_inputs,
             call_output.front(),
             /*opaque=*/opaque,
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);

  // TODO(penporn): Expect an OK status when FFI handlers support tokens.
  auto status = Execute(&b, {}).status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
  EXPECT_THAT(status.message(),
              ::testing::HasSubstr("FFI handlers do not support tokens"));
}

INSTANTIATE_TEST_SUITE_P(CustomCallTokensTest, CustomCallTokensTest,
                         ::testing::ValuesIn(GetTokenTestCases()));

static absl::Status AlwaysSucceed(ffi::BufferBase) { return absl::OkStatus(); }

XLA_FFI_DEFINE_HANDLER(kAlwaysSucceed, AlwaysSucceed,
                       ffi::Ffi::Bind().Arg<ffi::BufferBase>());

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(), "__xla_test$$always_succeed",
                         PLATFORM, kAlwaysSucceed);

TEST_F(CustomCallTest, ExportedFfiWithStatusSucceeded) {
  XlaBuilder b(TestName());
  CustomCall(&b, "__xla_test$$always_succeed", /*operands=*/{},
             ShapeUtil::MakeShape(F32, {}), /*opaque=*/"",
             /*has_side_effect=*/false,
             /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
             /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
             /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  TF_ASSERT_OK(Execute(&b, {}).status());
}

//===----------------------------------------------------------------------===//
// XLA:FFI handler with attached HloComputation
//===----------------------------------------------------------------------===//

static absl::Status MemcpyWithCalledComputation(
    se::Stream* stream, se::OwningScratchAllocator<> scratch_allocator,
    ffi::BufferBase src, ffi::BufferBase dst,
    const HloComputation* called_computation) {
  if (called_computation == nullptr)
    return absl::InternalError("Called computation is not defined");

  if (called_computation->instruction_count() != 1)
    return absl::InternalError("Unexpected number of instructions");

  if (!DynCast<HloParameterInstruction>(called_computation->root_instruction()))
    return absl::InternalError("ROOT must be a paremeter");

  return Memcpy(stream, src, dst);
}

XLA_FFI_DEFINE_HANDLER(kMemcpyWithCalledComputation,
                       MemcpyWithCalledComputation,
                       ffi::Ffi::Bind()
                           .Ctx<se::Stream>()
                           .Ctx<se::OwningScratchAllocator<>>()  // scratch
                           .Arg<ffi::BufferBase>()               // src
                           .Arg<ffi::BufferBase>()               // dst
                           .Ctx<ffi::CalledComputation>());

XLA_FFI_REGISTER_HANDLER(ffi::GetXlaFfiApi(),
                         "xla.gpu.ext.memcpy_with_called_computation", PLATFORM,
                         kMemcpyWithCalledComputation);

TEST_F(CustomCallTest, WithCalledComputation) {
  auto shape = ShapeUtil::MakeShape(F32, {128});

  // Build a called computation which is just a copy instruction.
  XlaBuilder copy("copy");
  auto p0 = Parameter(&copy, 0, shape, "l_val");
  Copy(p0);
  auto copy_computation = copy.Build().value();

  XlaBuilder b(TestName());
  CustomCallWithComputation(
      &b, "xla.gpu.ext.memcpy_with_called_computation",
      /*operands=*/{Broadcast(ConstantR0WithType(&b, F32, 42.0), {128})},
      copy_computation, shape, /*opaque=*/"",
      /*has_side_effect=*/false,
      /*output_operand_aliasing=*/{}, /*literal=*/nullptr,
      /*schedule=*/CustomCallSchedule::SCHEDULE_NONE,
      /*api_version=*/CustomCallApiVersion::API_VERSION_TYPED_FFI);
  TF_ASSERT_OK_AND_ASSIGN(auto result, ExecuteAndTransfer(&b, {}));
  EXPECT_THAT(result.data<float>(), ::testing::Each(42));
}

}  // anonymous namespace
}  // namespace xla
