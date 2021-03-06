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

// Contains implementation of the Intel instruction encoding specification
// language.

#include "cpu_instructions/x86/encoding_specification.h"

#include <cstdint>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <utility>
#include "strings/string.h"

#include "cpu_instructions/proto/x86/encoding_specification.pb.h"
#include "cpu_instructions/proto/x86/instruction_encoding.pb.h"
#include "glog/logging.h"
#include "re2/re2.h"
#include "strings/str_cat.h"
#include "strings/string_view.h"
#include "util/gtl/map_util.h"
#include "util/task/canonical_errors.h"
#include "util/task/status_macros.h"

namespace cpu_instructions {
namespace x86 {
namespace {

using ::cpu_instructions::util::InvalidArgumentError;
using ::cpu_instructions::util::OkStatus;
using ::cpu_instructions::util::Status;
using ::cpu_instructions::util::StatusOr;

// RE2 and protobuf have two slightly different implementations of StringPiece.
// In this file, we use RE2::Consume heavily, so we explicitly switch to the RE2
// implementation.
using ::re2::StringPiece;

bool ConsumePrefix(StringPiece* sp, StringPiece prefix) {
  DCHECK(sp != nullptr);
  if (!sp->starts_with(prefix)) return false;
  sp->remove_prefix(prefix.length());
  return true;
}

// The parser for the instruction encoding specification language used in the
// Intel manuals.
class EncodingSpecificationParser {
 public:
  // Initializes the instruction in a safe way. Call ParseFromString to fill the
  // structure with useful data.
  EncodingSpecificationParser();

  // Disallow copy and assign.
  EncodingSpecificationParser(const EncodingSpecificationParser&) = delete;
  EncodingSpecificationParser& operator=(const EncodingSpecificationParser&) =
      delete;

  // Parses the instruction data from string.
  StatusOr<EncodingSpecification> ParseFromString(StringPiece specification);

 private:
  // Resets the instruction to the initial state.
  void Reset();

  // Methods for parsing the prefixes of the instructions. There are two
  // separate methods - one parses VEX prefixes and the other parses the legacy
  // prefixes. Upon success, both methods advance 'specification' to the first
  // non-prefix byte. If the methods return a failure, the state of
  // 'specification' is undefined.
  Status ParseLegacyPrefixes(StringPiece* specification);
  Status ParseVexOrEvexPrefix(StringPiece* specification);

  // Parses the opcode of the instruction and its suffixes. Returns OK if the
  // opcode and the suffixes were parsed correctly, and if the specification
  // did not contain any additional data.
  // Expects that all prefixes were already consumed.
  Status ParseOpcodeAndSuffixes(StringPiece specification);

  // The current state of the parser.
  EncodingSpecification specification_;

  // Hash maps mapping tokens of the instruction encoding specification language
  // to enum values of the encoding protos.
  const std::unordered_map<string,
                           VexPrefixEncodingSpecification::VexOperandUsage>
      vex_operand_usage_tokens_;
  const std::unordered_map<string, VexPrefixEncodingSpecification::VectorSize>
      vector_size_tokens_;
  const std::unordered_map<string, VexEncoding::MandatoryPrefix>
      mandatory_prefix_tokens_;
  const std::unordered_map<string, VexPrefixEncodingSpecification::VexWUsage>
      vex_w_usage_tokens_;
  const std::unordered_map<uint32_t, VexEncoding::MapSelect> map_select_tokens_;
};

// Definitions of maps from tokens to the enum values used in the instruction
// encoding specification proto. These values are used to initialize the hash
// maps used in the parser object.
const std::pair<const char*, VexPrefixEncodingSpecification::VexOperandUsage>
    kVexOperandUsageTokens[] = {
        {"", VexPrefixEncodingSpecification::NO_VEX_OPERAND_USAGE},
        {"NDS",
         VexPrefixEncodingSpecification::VEX_OPERAND_IS_FIRST_SOURCE_REGISTER},
        {"NDD",
         VexPrefixEncodingSpecification::VEX_OPERAND_IS_DESTINATION_REGISTER},
        {"DDS", VexPrefixEncodingSpecification::
                    VEX_OPERAND_IS_SECOND_SOURCE_REGISTER}};
const std::pair<const char*, VexPrefixEncodingSpecification::VectorSize>
    kVectorSizeTokens[] = {
        {"LZ", VexPrefixEncodingSpecification::VECTOR_SIZE_BIT_IS_ZERO},
        // The two following are undocumented. We assume that L0 is equivalent
        // to LZ, and extend the semantics to L1 naturally to mean "L must be
        // 1".
        {"L0", VexPrefixEncodingSpecification::VECTOR_SIZE_BIT_IS_ZERO},
        {"L1", VexPrefixEncodingSpecification::VECTOR_SIZE_BIT_IS_ONE},
        {"128", VexPrefixEncodingSpecification::VECTOR_SIZE_128_BIT},
        {"256", VexPrefixEncodingSpecification::VECTOR_SIZE_256_BIT},
        {"512", VexPrefixEncodingSpecification::VECTOR_SIZE_512_BIT},
        {"LIG", VexPrefixEncodingSpecification::VECTOR_SIZE_IS_IGNORED},
        {"LIG.128", VexPrefixEncodingSpecification::VECTOR_SIZE_128_BIT}};
const std::pair<const char*, VexEncoding::MandatoryPrefix>
    kMandatoryPrefixTokens[] = {
        {"", VexEncoding::NO_MANDATORY_PREFIX},
        {"66", VexEncoding::MANDATORY_PREFIX_OPERAND_SIZE_OVERRIDE},
        {"F2", VexEncoding::MANDATORY_PREFIX_REPNE},
        {"F3", VexEncoding::MANDATORY_PREFIX_REPE}};
const std::pair<const char*, VexPrefixEncodingSpecification::VexWUsage>
    kVexWUsageTokens[] = {
        {"", VexPrefixEncodingSpecification::VEX_W_IS_IGNORED},
        {"W0", VexPrefixEncodingSpecification::VEX_W_IS_ZERO},
        {"W1", VexPrefixEncodingSpecification::VEX_W_IS_ONE},
        {"WIG", VexPrefixEncodingSpecification::VEX_W_IS_IGNORED}};
const std::pair<uint32_t, VexEncoding::MapSelect> kMapSelectTokens[] = {
    {0x0f, VexEncoding::MAP_SELECT_0F},
    {0x0f3a, VexEncoding::MAP_SELECT_0F3A},
    {0x0f38, VexEncoding::MAP_SELECT_0F38}};

inline void ConsumeWhitespace(StringPiece* specification) {
  DCHECK(specification != nullptr);
  while (ConsumePrefix(specification, " ") ||
         ConsumePrefix(specification, "+")) {
  }
}

EncodingSpecificationParser::EncodingSpecificationParser()
    : vex_operand_usage_tokens_(std::begin(kVexOperandUsageTokens),
                                std::end(kVexOperandUsageTokens)),
      vector_size_tokens_(std::begin(kVectorSizeTokens),
                          std::end(kVectorSizeTokens)),
      mandatory_prefix_tokens_(std::begin(kMandatoryPrefixTokens),
                               std::end(kMandatoryPrefixTokens)),
      vex_w_usage_tokens_(std::begin(kVexWUsageTokens),
                          std::end(kVexWUsageTokens)),
      map_select_tokens_(std::begin(kMapSelectTokens),
                         std::end(kMapSelectTokens)) {}

StatusOr<EncodingSpecification> EncodingSpecificationParser::ParseFromString(
    StringPiece specification) {
  specification_.Clear();
  if (specification.starts_with("VEX.") || specification.starts_with("EVEX")) {
    RETURN_IF_ERROR(ParseVexOrEvexPrefix(&specification));
  } else {
    RETURN_IF_ERROR(ParseLegacyPrefixes(&specification));
  }
  RETURN_IF_ERROR(ParseOpcodeAndSuffixes(specification));
  return std::move(specification_);
}

Status EncodingSpecificationParser::ParseLegacyPrefixes(
    StringPiece* specification) {
  CHECK(specification != nullptr);
  // A regexp for parsing the legacy prefixes. For more details on the format,
  // see Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2:
  // Instruction Set Reference, A-Z, Section 3.1.1.1 (page 3.2).
  // The parser matches all the possible prefixes and removes them from the
  // specification. When the string does not match anymore, it assues that this
  // is the beginning of the opcode and switches to parsing the opcode.
  // NOTE(ondrasej): kLegacyPrefixRegex must be static - removing it causes an
  // internal compiler error on gcc 4.8.
  static constexpr char kLegacyPrefixRegex[] =
      " *(?:"                  // Optional whitespace before the prefix.
      "(66)|"                  // The operand size override prefix.
      "(67)|"                  // THe address size override prefix.
      "(F2)|"                  // The REPNE prefix.
      "(F3)|"                  // The REPE prefix.
      "(REX(?:\\.(?:R|W))?))"  // The REX prefix. The manual uses this prefix in
                               // several forms: REX.W and REX.R to signal that
                               // a specific bit of the REX prefix is required,
                               // or just REX which probably implies REX.W.
      "(?: *\\+ *)?";          // Consume also any whitespace at the end.
  static const LazyRE2 legacy_prefix_parser = {kLegacyPrefixRegex};
  string operand_size_override_prefix;
  string address_size_override_prefix;
  string repne_prefix;
  string repe_prefix;
  string rex_prefix;
  bool has_mandatory_address_size_override_prefix = false;
  bool has_mandatory_operand_size_override_prefix = false;
  bool has_mandatory_repe_prefix = false;
  bool has_mandatory_repne_prefix = false;
  bool has_mandatory_rex_prefix = false;
  while (RE2::Consume(specification, *legacy_prefix_parser,
                      &operand_size_override_prefix,
                      &address_size_override_prefix, &repne_prefix,
                      &repe_prefix, &rex_prefix)) {
    has_mandatory_operand_size_override_prefix |=
        !operand_size_override_prefix.empty();
    has_mandatory_address_size_override_prefix |=
        !address_size_override_prefix.empty();
    has_mandatory_repe_prefix |= !repe_prefix.empty();
    has_mandatory_repne_prefix |= !repne_prefix.empty();
    has_mandatory_rex_prefix |= !rex_prefix.empty();
  }
  // Note that just calling mutable_legacy_prefixes will create an empty
  // legacy_prefixes field of the specification. This is desirable, because it
  // lets us make a difference between legacy instructions and VEX-encoded
  // instructions.
  LegacyPrefixEncodingSpecification* const legacy_prefixes =
      specification_.mutable_legacy_prefixes();
  legacy_prefixes->set_has_mandatory_operand_size_override_prefix(
      has_mandatory_operand_size_override_prefix);
  legacy_prefixes->set_has_mandatory_address_size_override_prefix(
      has_mandatory_address_size_override_prefix);
  legacy_prefixes->set_has_mandatory_repe_prefix(has_mandatory_repe_prefix);
  legacy_prefixes->set_has_mandatory_repne_prefix(has_mandatory_repne_prefix);
  legacy_prefixes->set_has_mandatory_rex_w_prefix(has_mandatory_rex_prefix);
  return OkStatus();
}

Status EncodingSpecificationParser::ParseVexOrEvexPrefix(
    StringPiece* specification) {
  CHECK(specification != nullptr);
  // A regexp for parsing the VEX prefix specification. For more details on the
  // format see Intel 64 and IA-32 Architectures Software Developer's Manual,
  // Volume 2: Instruction Set Reference, A-Z, Section 3.1.1.2 (page 3.3).
  // NOTE(ondrasej): Note that some of the fields do not affect the size of the
  // instruction encoding, so we just check that they have a valid value, but we
  // do not extract this value out of the regexp.
  // NOTE(ondrasej): kVexPrefixRegexp must be static - removing it causes an
  // internal compiler error on gcc 4.8.
  static constexpr char kVexPrefixRegexp[] =
      "(E?VEX)"                    // The VEX prefix.
      "(?: *\\. *(NDS|NDD|DDS))?"  // The directionality of the operand(s).
      "(?: *\\. *(LIG|LZ|L0|L1|LIG\\.128|128|256|512))?"  // Interpretation of
                                                          // the VEX and EVEX
                                                          // L/L' bits.
      "(?: *\\. *(66|F2|F3))?"     // The mandatory prefixes.
      " *\\. *(0F|0F3A|0F38)"      // The opcode prefix based on VEX.mmmmm.
      "(?: *\\. *(W0|W1|WIG))? ";  // Interpretation of the VEX.W bit.
  VexPrefixEncodingSpecification* const vex_prefix =
      specification_.mutable_vex_prefix();

  static const LazyRE2 vex_prefix_parser = {kVexPrefixRegexp};
  string prefix_type_str;
  string vex_operand_directionality;
  string vex_l_usage_str;
  string mandatory_prefix_str;
  uint32_t opcode_map;
  string vex_w_str;
  if (!RE2::Consume(specification, *vex_prefix_parser, &prefix_type_str,
                    &vex_operand_directionality, &vex_l_usage_str,
                    &mandatory_prefix_str, RE2::Hex(&opcode_map), &vex_w_str)) {
    return InvalidArgumentError(StrCat("Could not parse the VEX prefix: '",
                                       specification->ToString(), "'"));
  }

  // Parse the fields of the VEX prefix specification.
  // Note that we use the regexp to filter out invalid values of these fields,
  // so FindOrDie never dies unless the regexp and the token definitions at the
  // top of this file are out of sync.
  const VexPrefixEncodingSpecification::VexPrefixType prefix_type =
      prefix_type_str == "EVEX" ? VexPrefixEncodingSpecification::EVEX_PREFIX
                                : VexPrefixEncodingSpecification::VEX_PREFIX;
  const VexPrefixEncodingSpecification::VectorSize vector_size =
      FindOrDie(vector_size_tokens_, vex_l_usage_str);
  vex_prefix->set_prefix_type(prefix_type);
  vex_prefix->set_vex_operand_usage(
      FindOrDie(vex_operand_usage_tokens_, vex_operand_directionality));
  vex_prefix->set_vector_size(vector_size);
  if (vector_size == VexPrefixEncodingSpecification::VECTOR_SIZE_512_BIT &&
      prefix_type != VexPrefixEncodingSpecification::EVEX_PREFIX) {
    return InvalidArgumentError(
        "The 512 bit vector size can be used only in an EVEX prefix");
  }
  vex_prefix->set_mandatory_prefix(
      FindOrDie(mandatory_prefix_tokens_, mandatory_prefix_str));
  vex_prefix->set_vex_w_usage(FindOrDie(vex_w_usage_tokens_, vex_w_str));
  vex_prefix->set_map_select(FindOrDie(map_select_tokens_, opcode_map));

  // NOTE(ondrasej): The string specification of the opcode map is an equivalent
  // of opcode prefixes in the legacy encoding, and not the actual value used in
  // the VEX.mmmmm bits. This works to our advantage here, because we can simply
  // add it to the opcode.
  specification_.set_opcode(opcode_map);

  return OkStatus();
}

Status EncodingSpecificationParser::ParseOpcodeAndSuffixes(
    StringPiece specification) {
  VLOG(1) << "Parsing opcode and suffixes: " << specification;
  // We've already dealt with all possible prefixes. The rest are either
  // 1. a sequence of bytes (separated by space) of the opcode, in uppercase
  //    hex format, or
  // 2. information about the ModR/M bytes and immediate values.
  // The ModR/M info and immediate values have a fixed position, but
  // both of these are easy to tell from each other, so we can just parse them
  // in a for loop.
  static const LazyRE2 opcode_byte_parser = {
      " *([0-9A-F]{2})(?: *\\+ *(i|rb|rw|rd|ro))?"};
  int opcode_byte = 0;
  int num_opcode_bytes = 0;
  string opcode_encoded_register;
  uint32_t opcode = specification_.opcode();
  while (RE2::Consume(&specification, *opcode_byte_parser,
                      RE2::Hex(&opcode_byte), &opcode_encoded_register)) {
    ++num_opcode_bytes;
    opcode = (opcode << 8) | opcode_byte;
    if (!opcode_encoded_register.empty()) {
      if (opcode_encoded_register[0] == 'i') {
        specification_.set_operand_in_opcode(
            EncodingSpecification::FP_STACK_REGISTER_IN_OPCODE);
      } else {
        specification_.set_operand_in_opcode(
            EncodingSpecification::GENERAL_PURPOSE_REGISTER_IN_OPCODE);
      }
    }
  }
  specification_.set_opcode(opcode);
  if (num_opcode_bytes == 0) {
    return InvalidArgumentError("The instruction did not have an opcode byte.");
  }
  if (specification_.has_vex_prefix() && num_opcode_bytes != 1) {
    return InvalidArgumentError(
        "Unexpected number of opcode bytes in a VEX-encoded instruction.");
  }

  if (specification.empty()) {
    // There is neither ModR/M byte nor an immediate value.
    return OkStatus();
  }

  VLOG(1) << "Parsing suffixes: " << specification;
  // The variable that receives the value must be string. If we used chars
  // directly, the matching would fail because RE2 doesn't know how to convert
  // an empty matching group to a char.
  string is4_suffix_str;
  string modrm_suffix_str;
  string vsib_suffix_str;
  string immediate_value_size_str;
  string code_offset_size_str;
  // Notes on the suffix regexp:
  // * There might be a m64/m128 suffix that is not explained in the Intel
  //   manuals, but that most likely means that the operand in the ModR/M byte
  //   must be a memory operand. In practice, I've never seen them without
  //   another ModR/M suffix, so we just ignore them here.
  static const LazyRE2 modrm_and_imm_parser = {
      " *(?:"
      "(\\/is4)|"   // is4
      "i([bwdo])|"  // immediate
      "/([r0-9])|"  // modrm
      "(/vsib)|"    // vsib
      "(?:m(?:64|128|256))|"
      "c([bwdpot]))"};  // code offset size
  while (RE2::Consume(&specification, *modrm_and_imm_parser, &is4_suffix_str,
                      &immediate_value_size_str, &modrm_suffix_str,
                      &vsib_suffix_str, &code_offset_size_str)) {
    VLOG(1) << "modrm_suffix = " << modrm_suffix_str;
    VLOG(1) << "immediate_value_size_str = " << immediate_value_size_str;
    VLOG(1) << "code_offset_size = " << code_offset_size_str;
    // Only one of the following if statements will actually be evaluated,
    // because RE2 clears both strings at the beginning of the call to Consume.
    if (!modrm_suffix_str.empty()) {
      // If there was a ModR/M specifier, parse the usage of the MODRM.reg
      // value.
      const char modrm_suffix = modrm_suffix_str[0];
      if (modrm_suffix == 'r') {
        specification_.set_modrm_usage(EncodingSpecification::FULL_MODRM);
      } else {
        specification_.set_modrm_usage(
            EncodingSpecification::OPCODE_EXTENSION_IN_MODRM);
        specification_.set_modrm_opcode_extension(modrm_suffix - '0');
      }
    } else if (!immediate_value_size_str.empty()) {
      // If there was an immediate value specifier, parse the size of the
      // immediate value.
      switch (immediate_value_size_str[0]) {
        case 'b':
          specification_.add_immediate_value_bytes(1);
          break;
        case 'w':
          specification_.add_immediate_value_bytes(2);
          break;
        case 'd':
          specification_.add_immediate_value_bytes(4);
          break;
        case 'o':
          specification_.add_immediate_value_bytes(8);
          break;
        default:
          return InvalidArgumentError(StrCat("Invalid immediate value size: ",
                                             immediate_value_size_str));
      }
    } else if (!code_offset_size_str.empty()) {
      switch (code_offset_size_str[0]) {
        case 'b':
          specification_.set_code_offset_bytes(1);
          break;
        case 'w':
          specification_.set_code_offset_bytes(2);
          break;
        case 'd':
          specification_.set_code_offset_bytes(4);
          break;
        case 'p':
          specification_.set_code_offset_bytes(6);
          break;
        case 'o':
          specification_.set_code_offset_bytes(8);
          break;
        case 't':
          specification_.set_code_offset_bytes(10);
          break;
        default:
          return InvalidArgumentError(
              StrCat("Invalid code offset size: ", immediate_value_size_str));
      }
    } else if (!is4_suffix_str.empty()) {
      CHECK_EQ("/is4", is4_suffix_str);
      if (!specification_.has_vex_prefix()) {
        return InvalidArgumentError(
            "The VEX operand suffix /is4 is specified for an instruction that "
            "does not use the VEX prefix.");
      }
      specification_.mutable_vex_prefix()->set_has_vex_operand_suffix(true);
    } else if (!vsib_suffix_str.empty()) {
      CHECK_EQ("/vsib", vsib_suffix_str);
      if (!specification_.has_vex_prefix()) {
        return InvalidArgumentError(
            "The VEX operand suffix /vsib is specified for an instruction that "
            "does not use the VEX prefix.");
      }
      specification_.mutable_vex_prefix()->set_vsib_usage(
          VexPrefixEncodingSpecification::VSIB_USED);
    }
  }

  // VSIB implies that ModRM is used: ModRM.rm has to be 0b100, and ModRM.reg
  // can be used to encode either an extra operand or an opcode extension.
  if (specification_.vex_prefix().vsib_usage() ==
          VexPrefixEncodingSpecification::VSIB_USED &&
      specification_.modrm_usage() == EncodingSpecification::NO_MODRM_USAGE) {
    specification_.set_modrm_usage(EncodingSpecification::FULL_MODRM);
  }

  ConsumeWhitespace(&specification);
  return specification.empty() ? OkStatus()
                               : InvalidArgumentError(StrCat(
                                     "The specification was not fully parsed: ",
                                     specification.ToString()));
}

}  // namespace

StatusOr<EncodingSpecification> ParseEncodingSpecification(
    const string& specification) {
  EncodingSpecificationParser parser;
  return parser.ParseFromString(specification);
}

InstructionOperandEncodingMultiset GetAvailableEncodings(
    const EncodingSpecification& encoding_specification) {
  InstructionOperandEncodingMultiset available_encodings;
  // If the instruction uses ModR/M byte, the operands might be encoded using
  // some of the ModR/M byte fields.
  switch (encoding_specification.modrm_usage()) {
    case EncodingSpecification::FULL_MODRM:
      available_encodings.insert(InstructionOperand::MODRM_REG_ENCODING);
      available_encodings.insert(InstructionOperand::MODRM_RM_ENCODING);
      break;
    case EncodingSpecification::OPCODE_EXTENSION_IN_MODRM:
      available_encodings.insert(InstructionOperand::MODRM_RM_ENCODING);
      break;
    default:
      break;
  }
  // If the instruction uses opcode bits to encode the operands, the operand
  // might be encoded using the opcode bits.
  if (encoding_specification.operand_in_opcode() !=
      EncodingSpecification::NO_OPERAND_IN_OPCODE) {
    available_encodings.insert(InstructionOperand::OPCODE_ENCODING);
  }
  // If the instruction uses the VEX prefix, the operands might be encoded in
  // the VEX.vvvv bits.
  if (encoding_specification.has_vex_prefix()) {
    const VexPrefixEncodingSpecification& vex_prefix =
        encoding_specification.vex_prefix();
    if (vex_prefix.vex_operand_usage() !=
        VexPrefixEncodingSpecification::NO_VEX_OPERAND_USAGE) {
      available_encodings.insert(InstructionOperand::VEX_V_ENCODING);
    }
    if (vex_prefix.has_vex_operand_suffix()) {
      available_encodings.insert(InstructionOperand::VEX_SUFFIX_ENCODING);
    }
    if (vex_prefix.vsib_usage() !=
        VexPrefixEncodingSpecification::VSIB_UNUSED) {
      available_encodings.insert(InstructionOperand::VSIB_ENCODING);
      // See comment in ParseOpcodeAndSuffixes().
      CHECK_NE(encoding_specification.modrm_usage(),
               EncodingSpecification::NO_MODRM_USAGE)
          << encoding_specification.DebugString();
      // VSIB requires ModRM.rm to be 0b100, so it cannot be used to encoed an
      // operand.
      available_encodings.erase(InstructionOperand::MODRM_RM_ENCODING);
    }
  }
  // Add implicit encodings for implicit operands.
  const int num_implicit_operands =
      encoding_specification.immediate_value_bytes_size() +
      (encoding_specification.code_offset_bytes() > 0 ? 1 : 0);
  for (int i = 0; i < num_implicit_operands; ++i) {
    available_encodings.insert(InstructionOperand::IMMEDIATE_VALUE_ENCODING);
  }
  return available_encodings;
}

}  // namespace x86
}  // namespace cpu_instructions
