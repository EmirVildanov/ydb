/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_INTERNAL_COMPILER_PYTHON_GENERATOR_H
#define GRPC_INTERNAL_COMPILER_PYTHON_GENERATOR_H

#include <utility>
#include <vector>

#include "src/compiler/config.h"
#include "src/compiler/schema_interface.h"

namespace grpc_python_generator {

// Data pertaining to configuration of the generator with respect to anything
// that may be used internally at Google.
struct GeneratorConfiguration {
  GeneratorConfiguration();
  TString grpc_package_root;
  // TODO(https://github.com/grpc/grpc/issues/8622): Drop this.
  TString beta_package_root;
  // TODO(https://github.com/google/protobuf/issues/888): Drop this.
  TString import_prefix;
  std::vector<TString> prefixes_to_filter;
};

class PythonGrpcGenerator : public grpc::protobuf::compiler::CodeGenerator {
 public:
  PythonGrpcGenerator(const GeneratorConfiguration& config);
  ~PythonGrpcGenerator();

  uint64_t GetSupportedFeatures() const override;

  bool Generate(const grpc::protobuf::FileDescriptor* file,
                const TString& parameter,
                grpc::protobuf::compiler::GeneratorContext* context,
                TString* error) const override;

 private:
  GeneratorConfiguration config_;
};

}  // namespace grpc_python_generator

#endif  // GRPC_INTERNAL_COMPILER_PYTHON_GENERATOR_H
