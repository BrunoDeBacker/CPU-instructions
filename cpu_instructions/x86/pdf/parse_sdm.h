// Copyright 2016 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CPU_INSTRUCTIONS_X86_PDF_PARSE_SDM_H_
#define CPU_INSTRUCTIONS_X86_PDF_PARSE_SDM_H_

#include "strings/string.h"

#include "cpu_instructions/proto/instructions.pb.h"

namespace cpu_instructions {
namespace x86 {
namespace pdf {

// Parses the Intel SDM. Input is specified in input_spec. Outputs are:
//   - The parsed database of instructions, written to <output_base>.pbtxt
//   - Two raw protos per input file for debug, with the contents
//     of the PDF (raw parsed input) and SDM (interpreted input) respectively,
//     as <output_base>_<input_id>.{pdf,sdm}.pb
// The patches contained in patch_sets_file are applied before interpreting the
// SDM.
InstructionSetProto ParseSdmOrDie(const string& input_spec,
                                  const string& patch_sets_file,
                                  const string& output_base);

}  // namespace pdf
}  // namespace x86
}  // namespace cpu_instructions

#endif  // CPU_INSTRUCTIONS_X86_PDF_PARSE_SDM_H_
