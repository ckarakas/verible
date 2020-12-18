// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verilog/analysis/symbol_table.h"

#include <functional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "common/text/tree_utils.h"
#include "common/util/logging.h"
#include "common/util/range.h"
#include "verilog/analysis/verilog_project.h"

namespace verilog {

// Directly test some SymbolTable internals.
class SymbolTable::Tester : public SymbolTable {
 public:
  explicit Tester(VerilogProject* project) : SymbolTable(project) {}

  using SymbolTable::MutableRoot;
};

namespace {

using testing::HasSubstr;

// Grab the first reference in local_references_to_bind that matches by name.
static const DependentReferences& GetFirstDependentReferenceByName(
    const SymbolInfo& sym, absl::string_view name) {
  for (const auto& ref : sym.local_references_to_bind) {
    if (ref.components->Value().identifier == name) return ref;
  }
  CHECK(false) << "No reference to \"" << name << "\" found.";
}

// An in-memory source file that doesn't require file-system access,
// nor create temporary files.
// TODO: if this is useful elsewhere, move to a test-only library.
class TestVerilogSourceFile : public VerilogSourceFile {
 public:
  // filename can be fake, it is not used to open any file.
  TestVerilogSourceFile(absl::string_view filename, absl::string_view contents)
      : VerilogSourceFile(filename, filename), contents_for_open_(contents) {}

  // Load text into analyzer structure without actually opening a file.
  absl::Status Open() override {
    analyzed_structure_ = ABSL_DIE_IF_NULL(
        absl::make_unique<VerilogAnalyzer>(contents_for_open_, ResolvedPath()));
    state_ = State::kOpened;
    status_ = absl::OkStatus();
    return status_;
  }

 private:
  const absl::string_view contents_for_open_;
};

struct ScopePathPrinter {
  const SymbolTableNode& node;
};

static std::ostream& operator<<(std::ostream& stream,
                                const ScopePathPrinter& p) {
  return SymbolTableNodeFullPath(stream, p.node);
}

// Shorthand for asserting that a symbol table lookup from
// (const SymbolTableNode& scope) using (absl::string_view key) must succeed,
// and is captured as (const SymbolTableNode& dest).
// Most of the time, the tester is not interested in the found_* iterator.
// This also defines 'dest##_info' as the SymbolInfo value attached to the
// 'dest' SymbolTableNode. Defined as a macro so that failure gives meaningful
// line numbers, and this allows ASSERT_NE to early exit.
#define MUST_ASSIGN_LOOKUP_SYMBOL(dest, scope, key)                       \
  const auto found_##dest = scope.Find(key);                              \
  ASSERT_NE(found_##dest, scope.end())                                    \
      << "No symbol at \"" << key << "\" in " << ScopePathPrinter{scope}; \
  EXPECT_EQ(found_##dest->first, key);                                    \
  const SymbolTableNode& dest(found_##dest->second);                      \
  const SymbolInfo& dest##_info ABSL_ATTRIBUTE_UNUSED(dest.Value())

TEST(SymbolTableNodeFullPathTest, Print) {
  typedef SymbolTableNode::key_value_type KV;
  const SymbolTableNode root(
      SymbolInfo{},
      KV("AA", SymbolTableNode(SymbolInfo{}, KV("BB", SymbolTableNode{}))));
  {
    std::ostringstream stream;
    SymbolTableNodeFullPath(stream, root);
    EXPECT_EQ(stream.str(), "$root");
  }
  {
    std::ostringstream stream;
    SymbolTableNodeFullPath(stream, root.begin()->second);
    EXPECT_EQ(stream.str(), "$root::AA");
  }
  {
    std::ostringstream stream;
    SymbolTableNodeFullPath(stream, root.begin()->second.begin()->second);
    EXPECT_EQ(stream.str(), "$root::AA::BB");
  }
}

TEST(ReferenceNodeFullPathTest, Print) {
  typedef ReferenceComponentNode Node;
  typedef ReferenceComponent Data;
  const Node root(
      Data{.identifier = "xx", .ref_type = ReferenceType::kUnqualified},
      Node(Data{.identifier = "yy", .ref_type = ReferenceType::kStaticMember},
           Node(Data{.identifier = "zz",
                     .ref_type = ReferenceType::kObjectMember})));
  {
    std::ostringstream stream;
    ReferenceNodeFullPath(stream, root);
    EXPECT_EQ(stream.str(), "@xx");
  }
  {
    std::ostringstream stream;
    ReferenceNodeFullPath(stream, root.Children().front());
    EXPECT_EQ(stream.str(), "@xx::yy");
  }
  {
    std::ostringstream stream;
    ReferenceNodeFullPath(stream, root.Children().front().Children().front());
    EXPECT_EQ(stream.str(), "@xx::yy.zz");
  }
}

TEST(BuildSymbolTableTest, IntegrityCheckResolvedSymbol) {
  const auto test_func = []() {
    SymbolTable::Tester symbol_table_1(nullptr), symbol_table_2(nullptr);
    SymbolTableNode& root1(symbol_table_1.MutableRoot());
    SymbolTableNode& root2(symbol_table_2.MutableRoot());
    // Deliberately point from one symbol table to the other.
    root1.Value().local_references_to_bind.push_back(DependentReferences{
        .components = absl::make_unique<ReferenceComponentNode>(
            ReferenceComponent{.identifier = "foo",
                               .ref_type = ReferenceType::kUnqualified,
                               .resolved_symbol = &root2})});
    // CheckIntegrity() will fail on destruction of symbol_table_1.
  };
  EXPECT_DEATH(test_func(),
               "Resolved symbols must point to a node in the same SymbolTable");
}

TEST(BuildSymbolTableTest, IntegrityCheckDeclaredType) {
  const auto test_func = []() {
    SymbolTable::Tester symbol_table_1(nullptr), symbol_table_2(nullptr);
    SymbolTableNode& root1(symbol_table_1.MutableRoot());
    SymbolTableNode& root2(symbol_table_2.MutableRoot());
    // Deliberately point from one symbol table to the other.
    root1.Value().local_references_to_bind.push_back(DependentReferences{
        .components = absl::make_unique<ReferenceComponentNode>(
            ReferenceComponent{.identifier = "foo",
                               .ref_type = ReferenceType::kUnqualified,
                               .resolved_symbol = &root1})});
    root2.Value().declared_type.user_defined_type =
        root1.Value().local_references_to_bind.front().components.get();
    // CheckIntegrity() will fail on destruction of symbol_table_2.
  };
  EXPECT_DEATH(test_func(),
               "Resolved symbols must point to a node in the same SymbolTable");
}

TEST(BuildSymbolTableTest, InvalidSyntax) {
  TestVerilogSourceFile src("foobar.sv", "module;\nendmodule\n");
  const auto status = src.Parse();
  EXPECT_FALSE(status.ok());
  SymbolTable symbol_table(nullptr);

  {  // Attempt to build symbol table after parse failure.
    const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

    EXPECT_TRUE(symbol_table.Root().Children().empty());
    EXPECT_TRUE(build_diagnostics.empty())
        << "Unexpected diagnostic:\n"
        << build_diagnostics.front().message();
  }
  {  // Attempt to resolve empty symbol table and references.
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationSingleEmpty) {
  TestVerilogSourceFile src("foobar.sv", "module m;\nendmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "m");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);
  EXPECT_EQ(module_node_info.file_origin, &src);
  EXPECT_EQ(module_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationLocalNetsVariables) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m;\n"
                            "  wire w1, w2;\n"
                            "  logic l1, l2;\n"
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "m");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);
  EXPECT_EQ(module_node_info.file_origin, &src);
  EXPECT_EQ(module_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  static constexpr absl::string_view members[] = {"w1", "w2", "l1", "l2"};
  for (const auto& member : members) {
    MUST_ASSIGN_LOOKUP_SYMBOL(member_node, module_node, member);
    EXPECT_EQ(member_node_info.type, SymbolType::kDataNetVariableInstance);
    EXPECT_EQ(member_node_info.declared_type.user_defined_type,
              nullptr);  // types are primitive
  }

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationLocalDuplicateNets) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m;\n"
                            "  wire y1;\n"
                            "  logic y1;\n"  // y1 already declared
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "m");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);
  EXPECT_EQ(module_node_info.file_origin, &src);
  EXPECT_EQ(module_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  ASSERT_EQ(build_diagnostics.size(), 1);
  const absl::Status& err_status(build_diagnostics.front());
  EXPECT_EQ(err_status.code(), absl::StatusCode::kAlreadyExists);
  EXPECT_THAT(err_status.message(),
              HasSubstr("\"y1\" is already defined in the $root::m scope"));

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationWithPorts) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m (\n"
                            "  input wire clk,\n"
                            "  output reg q\n"
                            ");\n"
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "m");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);
  EXPECT_EQ(module_node_info.file_origin, &src);
  EXPECT_EQ(module_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  static constexpr absl::string_view members[] = {"clk", "q"};
  for (const auto& member : members) {
    MUST_ASSIGN_LOOKUP_SYMBOL(member_node, module_node, member);
    EXPECT_EQ(member_node_info.type, SymbolType::kDataNetVariableInstance);
    EXPECT_EQ(member_node_info.declared_type.user_defined_type,
              nullptr);  // types are primitive
  }

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationMultiple) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m1;\nendmodule\n"
                            "module m2;\nendmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  const absl::string_view expected_modules[] = {"m1", "m2"};
  for (const auto& expected_module : expected_modules) {
    MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, expected_module);
    EXPECT_EQ(module_node_info.type, SymbolType::kModule);
    EXPECT_EQ(module_node_info.file_origin, &src);
    EXPECT_EQ(module_node_info.declared_type.syntax_origin,
              nullptr);  // there is no module meta-type
    EXPECT_TRUE(build_diagnostics.empty())
        << "Unexpected diagnostic:\n"
        << build_diagnostics.front().message();
  }

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationDuplicate) {
  TestVerilogSourceFile src("foobar.sv",
                            "module mm;\nendmodule\n"
                            "module mm;\nendmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "mm");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);
  EXPECT_EQ(module_node_info.file_origin, &src);
  EXPECT_EQ(module_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  ASSERT_EQ(build_diagnostics.size(), 1);
  EXPECT_EQ(build_diagnostics.front().code(), absl::StatusCode::kAlreadyExists);
  EXPECT_THAT(build_diagnostics.front().message(),
              HasSubstr("\"mm\" is already defined in the $root scope"));

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationNested) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m_outer;\n"
                            "module m_inner;\n"
                            "endmodule\n"
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();
  MUST_ASSIGN_LOOKUP_SYMBOL(outer_module_node, root_symbol, "m_outer");
  {
    EXPECT_EQ(outer_module_node_info.type, SymbolType::kModule);
    EXPECT_EQ(outer_module_node_info.file_origin, &src);
    EXPECT_EQ(outer_module_node_info.declared_type.syntax_origin,
              nullptr);  // there is no module meta-type
  }
  {
    MUST_ASSIGN_LOOKUP_SYMBOL(inner_module_node, outer_module_node, "m_inner");
    EXPECT_EQ(inner_module_node_info.type, SymbolType::kModule);
    EXPECT_EQ(inner_module_node_info.file_origin, &src);
    EXPECT_EQ(inner_module_node_info.declared_type.syntax_origin,
              nullptr);  // there is no module meta-type
  }
  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleDeclarationNestedDuplicate) {
  TestVerilogSourceFile src("foobar.sv",
                            "module outer;\n"
                            "module mm;\nendmodule\n"
                            "module mm;\nendmodule\n"  // dupe
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(module_node, root_symbol, "outer");
  EXPECT_EQ(module_node_info.type, SymbolType::kModule);

  ASSERT_EQ(build_diagnostics.size(), 1);
  EXPECT_EQ(build_diagnostics.front().code(), absl::StatusCode::kAlreadyExists);
  EXPECT_THAT(build_diagnostics.front().message(),
              HasSubstr("\"mm\" is already defined in the $root::outer scope"));
  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ModuleInstance) {
  // The following code variants should yield the same symbol table results:
  static constexpr absl::string_view source_variants[] = {
      // pp defined earlier in file
      "module pp;\n"
      "endmodule\n"
      "module qq;\n"
      "  pp rr();\n"  // instance
      "endmodule\n",
      // pp defined later in file
      "module qq;\n"
      "  pp rr();\n"  // instance
      "endmodule\n"
      "module pp;\n"
      "endmodule\n",
  };
  for (const auto& code : source_variants) {
    VLOG(1) << "code:\n" << code;
    TestVerilogSourceFile src("foobar.sv", code);
    const auto status = src.Parse();
    ASSERT_TRUE(status.ok()) << status.message();
    SymbolTable symbol_table(nullptr);
    const SymbolTableNode& root_symbol(symbol_table.Root());

    const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

    EXPECT_TRUE(build_diagnostics.empty())
        << "Unexpected diagnostic:\n"
        << build_diagnostics.front().message();

    // Goal: resolve the reference of "pp" to this definition node.
    MUST_ASSIGN_LOOKUP_SYMBOL(pp, root_symbol, "pp");

    // Inspect inside the "qq" module definition.
    MUST_ASSIGN_LOOKUP_SYMBOL(qq, root_symbol, "qq");

    // "rr" is an instance of type "pp"
    MUST_ASSIGN_LOOKUP_SYMBOL(rr, qq, "rr");

    {
      // There is only one reference, and it is to the "pp" module type.
      ASSERT_EQ(qq_info.local_references_to_bind.size(), 2);
      EXPECT_EQ(qq_info.file_origin, &src);
      {
        const DependentReferences& pp_type(
            GetFirstDependentReferenceByName(qq_info, "pp"));
        const ReferenceComponentNode* ref_node = pp_type.LastLeaf();
        ASSERT_NE(ref_node, nullptr);
        const ReferenceComponent& ref(ref_node->Value());
        EXPECT_EQ(ref.identifier, "pp");
        EXPECT_TRUE(verible::IsSubRange(ref.identifier,
                                        src.GetTextStructure()->Contents()));
        EXPECT_EQ(ref.ref_type, ReferenceType::kUnqualified);
        EXPECT_EQ(ref.resolved_symbol, nullptr);
      }
      {
        const DependentReferences& rr_self_ref(
            GetFirstDependentReferenceByName(qq_info, "rr"));
        EXPECT_TRUE(rr_self_ref.components->is_leaf());  // no named ports
        // self-reference is already bound.
        EXPECT_EQ(rr_self_ref.components->Value().resolved_symbol, &rr);
      }
    }

    EXPECT_TRUE(rr_info.local_references_to_bind.empty());
    EXPECT_NE(rr_info.declared_type.user_defined_type, nullptr);
    {
      const ReferenceComponent& pp_type(
          rr_info.declared_type.user_defined_type->Value());
      EXPECT_EQ(pp_type.identifier, "pp");
      EXPECT_EQ(pp_type.resolved_symbol, nullptr);  // nothing resolved yet
      EXPECT_EQ(pp_type.ref_type, ReferenceType::kUnqualified);
    }
    EXPECT_EQ(rr_info.file_origin, &src);

    // Resolve symbols.
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());
    // Verify that typeof(rr) successfully resolved to module pp.
    EXPECT_EQ(rr_info.declared_type.user_defined_type->Value().resolved_symbol,
              &pp);
  }
}

TEST(BuildSymbolTableTest, ModuleInstanceUndefined) {
  TestVerilogSourceFile src("foobar.sv",
                            "module qq;\n"
                            "  pp rr();\n"  // instance, pp undefined
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  // Inspect inside the "qq" module definition.
  MUST_ASSIGN_LOOKUP_SYMBOL(qq, root_symbol, "qq");
  {
    // There is only one reference of interest, the "pp" module type.
    ASSERT_EQ(qq_info.local_references_to_bind.size(), 2);
    EXPECT_EQ(qq_info.file_origin, &src);
    const DependentReferences& pp_type(
        GetFirstDependentReferenceByName(qq_info, "pp"));
    {  // verify that a reference to "pp" was established
      const ReferenceComponentNode* ref_node = pp_type.LastLeaf();
      ASSERT_NE(ref_node, nullptr);
      const ReferenceComponent& ref(ref_node->Value());
      EXPECT_EQ(ref.identifier, "pp");
      EXPECT_TRUE(verible::IsSubRange(ref.identifier,
                                      src.GetTextStructure()->Contents()));
      EXPECT_EQ(ref.ref_type, ReferenceType::kUnqualified);
      EXPECT_EQ(ref.resolved_symbol, nullptr);
    }
  }

  // "rr" is an instance of type "pp" (which is undefined)
  MUST_ASSIGN_LOOKUP_SYMBOL(rr, qq, "rr");
  EXPECT_TRUE(rr_info.local_references_to_bind.empty());
  EXPECT_NE(rr_info.declared_type.user_defined_type, nullptr);
  {
    const ReferenceComponent& pp_type(
        rr_info.declared_type.user_defined_type->Value());
    EXPECT_EQ(pp_type.identifier, "pp");
    EXPECT_EQ(pp_type.resolved_symbol, nullptr);  // nothing resolved yet
    EXPECT_EQ(pp_type.ref_type, ReferenceType::kUnqualified);
  }
  EXPECT_EQ(rr_info.file_origin, &src);

  {
    // Resolve symbols.  Expect one unresolved symbol.
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_EQ(resolve_diagnostics.size(), 1);
    const auto& err_status(resolve_diagnostics.front());
    EXPECT_EQ(err_status.code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(err_status.message(),
                HasSubstr("Unable to resolve symbol \"pp\""));
    // Verify that typeof(rr) failed to resolve "pp".
    EXPECT_EQ(rr_info.declared_type.user_defined_type->Value().resolved_symbol,
              nullptr);
  }
}

TEST(BuildSymbolTableTest, ModuleInstanceTwoInSameDecl) {
  static constexpr absl::string_view source_variants[] = {
      // The following all yield equivalent symbol tables bindings.
      "module pp;\n"
      "endmodule\n"
      "module qq;\n"
      "  pp r1(), r2();\n"  // instances
      "endmodule\n",
      "module qq;\n"
      "  pp r1(), r2();\n"  // instances
      "endmodule\n"
      "module pp;\n"
      "endmodule\n",
      // swap r1, r2 order
      "module pp;\n"
      "endmodule\n"
      "module qq;\n"
      "  pp r2(), r1();\n"  // instances
      "endmodule\n",
      "module qq;\n"
      "  pp r2(), r1();\n"  // instances
      "endmodule\n"
      "module pp;\n"
      "endmodule\n",
  };
  for (const auto& code : source_variants) {
    TestVerilogSourceFile src("foobar.sv", code);
    const auto status = src.Parse();
    ASSERT_TRUE(status.ok()) << status.message();
    SymbolTable symbol_table(nullptr);
    const SymbolTableNode& root_symbol(symbol_table.Root());

    const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

    EXPECT_TRUE(build_diagnostics.empty())
        << "Unexpected diagnostic:\n"
        << build_diagnostics.front().message();

    MUST_ASSIGN_LOOKUP_SYMBOL(pp, root_symbol, "pp");

    // Inspect inside the "qq" module definition.
    MUST_ASSIGN_LOOKUP_SYMBOL(qq, root_symbol, "qq");
    {
      // There is only one type reference of interest, the "pp" module type.
      // The other two are instance self-references.
      ASSERT_EQ(qq_info.local_references_to_bind.size(), 3);
      EXPECT_EQ(qq_info.file_origin, &src);
      {
        const DependentReferences& pp_type(
            GetFirstDependentReferenceByName(qq_info, "pp"));
        const ReferenceComponentNode* ref_node = pp_type.LastLeaf();
        ASSERT_NE(ref_node, nullptr);
        const ReferenceComponent& ref(ref_node->Value());
        EXPECT_EQ(ref.identifier, "pp");
        EXPECT_TRUE(verible::IsSubRange(ref.identifier,
                                        src.GetTextStructure()->Contents()));
        EXPECT_EQ(ref.ref_type, ReferenceType::kUnqualified);
        EXPECT_EQ(ref.resolved_symbol, nullptr);
      }
    }

    // "r1" and "r2" are both instances of type "pp"
    static constexpr absl::string_view pp_instances[] = {"r1", "r2"};
    for (const auto& pp_inst : pp_instances) {
      MUST_ASSIGN_LOOKUP_SYMBOL(rr, qq, pp_inst);
      EXPECT_TRUE(rr_info.local_references_to_bind.empty());
      EXPECT_NE(rr_info.declared_type.user_defined_type, nullptr);
      {
        const ReferenceComponent& pp_type(
            rr_info.declared_type.user_defined_type->Value());
        EXPECT_EQ(pp_type.identifier, "pp");
        EXPECT_EQ(pp_type.resolved_symbol, nullptr);  // nothing resolved yet
        EXPECT_EQ(pp_type.ref_type, ReferenceType::kUnqualified);
      }
      EXPECT_EQ(rr_info.file_origin, &src);
    }

    {
      std::vector<absl::Status> resolve_diagnostics;
      symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
      EXPECT_TRUE(resolve_diagnostics.empty());
      for (const auto& pp_inst : pp_instances) {
        MUST_ASSIGN_LOOKUP_SYMBOL(rr, qq, pp_inst);
        EXPECT_TRUE(rr_info.local_references_to_bind.empty());
        // Verify that typeof(rr) successfully resolved to module pp.
        EXPECT_EQ(
            rr_info.declared_type.user_defined_type->Value().resolved_symbol,
            &pp);
      }
    }
  }
}

TEST(BuildSymbolTableTest, ModuleInstancePositionalPortConnection) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m (\n"
                            "  input wire clk,\n"
                            "  output reg q\n"
                            ");\n"
                            "endmodule\n"
                            "module rr;\n"
                            "  wire c, d;\n"
                            "  m m_inst(c, d);"
                            // one type reference, two net references
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(m_node, root_symbol, "m");
  EXPECT_EQ(m_node_info.type, SymbolType::kModule);
  EXPECT_EQ(m_node_info.file_origin, &src);
  EXPECT_EQ(m_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  MUST_ASSIGN_LOOKUP_SYMBOL(clk_node, m_node, "clk");
  EXPECT_EQ(clk_node_info.type, SymbolType::kDataNetVariableInstance);
  EXPECT_EQ(clk_node_info.declared_type.user_defined_type,
            nullptr);  // types are primitive

  MUST_ASSIGN_LOOKUP_SYMBOL(q_node, m_node, "q");
  EXPECT_EQ(q_node_info.type, SymbolType::kDataNetVariableInstance);
  EXPECT_EQ(q_node_info.declared_type.user_defined_type,
            nullptr);  // types are primitive

  MUST_ASSIGN_LOOKUP_SYMBOL(rr_node, root_symbol, "rr");

  // Inspect local references to wires "c" and "d".
  ASSERT_EQ(rr_node_info.local_references_to_bind.size(), 4);
  // Sort references because there are no ordering guarantees among them.
  const DependentReferences* c_ref = nullptr;
  const DependentReferences* d_ref = nullptr;
  for (const auto& ref : rr_node_info.local_references_to_bind) {
    const absl::string_view base_ref(ref.components->Value().identifier);
    if (base_ref == "c") c_ref = &ref;
    if (base_ref == "d") d_ref = &ref;
  }
  ASSERT_NE(c_ref, nullptr);
  ASSERT_NE(d_ref, nullptr);
  EXPECT_EQ(c_ref->LastLeaf()->Value().identifier, "c");
  EXPECT_EQ(c_ref->LastLeaf()->Value().resolved_symbol, nullptr);
  EXPECT_EQ(d_ref->LastLeaf()->Value().identifier, "d");
  EXPECT_EQ(d_ref->LastLeaf()->Value().resolved_symbol, nullptr);

  // Get the local symbol definitions for wires "c" and "d".
  MUST_ASSIGN_LOOKUP_SYMBOL(c_node, rr_node, "c");
  MUST_ASSIGN_LOOKUP_SYMBOL(d_node, rr_node, "d");

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());

    // Expect to resolve local references to wires c and d
    EXPECT_EQ(c_ref->LastLeaf()->Value().resolved_symbol, &c_node);
    EXPECT_EQ(d_ref->LastLeaf()->Value().resolved_symbol, &d_node);
  }
}

TEST(BuildSymbolTableTest, ModuleInstanceNamedPortConnection) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m (\n"
                            "  input wire clk,\n"
                            "  output reg q\n"
                            ");\n"
                            "endmodule\n"
                            "module rr;\n"
                            "  wire c, d;\n"
                            "  m m_inst(.clk(c), .q(d));"
                            // one type reference, two local net references
                            // two named port references
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(m_node, root_symbol, "m");
  EXPECT_EQ(m_node_info.type, SymbolType::kModule);
  EXPECT_EQ(m_node_info.file_origin, &src);
  EXPECT_EQ(m_node_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  MUST_ASSIGN_LOOKUP_SYMBOL(clk_node, m_node, "clk");
  EXPECT_EQ(clk_node_info.type, SymbolType::kDataNetVariableInstance);
  EXPECT_EQ(clk_node_info.declared_type.user_defined_type,
            nullptr);  // types are primitive

  MUST_ASSIGN_LOOKUP_SYMBOL(q_node, m_node, "q");
  EXPECT_EQ(q_node_info.type, SymbolType::kDataNetVariableInstance);
  EXPECT_EQ(q_node_info.declared_type.user_defined_type,
            nullptr);  // types are primitive

  MUST_ASSIGN_LOOKUP_SYMBOL(rr_node, root_symbol, "rr");

  // Inspect local references to wires "c" and "d".
  ASSERT_EQ(rr_node_info.local_references_to_bind.size(), 4);
  // Sort references because there are no ordering guarantees among them.
  const DependentReferences* c_ref = nullptr;
  const DependentReferences* d_ref = nullptr;
  const DependentReferences* m_inst_ref = nullptr;
  for (const auto& ref : rr_node_info.local_references_to_bind) {
    const absl::string_view base_ref(ref.components->Value().identifier);
    if (base_ref == "c") c_ref = &ref;
    if (base_ref == "d") d_ref = &ref;
    if (base_ref == "m_inst") m_inst_ref = &ref;  // instance self-reference
  }
  ASSERT_NE(c_ref, nullptr);
  ASSERT_NE(d_ref, nullptr);
  ASSERT_NE(m_inst_ref, nullptr);
  EXPECT_EQ(c_ref->LastLeaf()->Value().identifier, "c");
  EXPECT_EQ(c_ref->LastLeaf()->Value().resolved_symbol, nullptr);
  EXPECT_EQ(d_ref->LastLeaf()->Value().identifier, "d");
  EXPECT_EQ(d_ref->LastLeaf()->Value().resolved_symbol, nullptr);

  const ReferenceComponentNode& m_inst_ref_root(*m_inst_ref->components);
  ASSERT_EQ(m_inst_ref_root.Children().size(), 2);
  const ReferenceComponentMap port_refs(
      ReferenceComponentNodeMapView(m_inst_ref_root));

  const auto found_clk_ref = port_refs.find("clk");
  ASSERT_NE(found_clk_ref, port_refs.end());
  const ReferenceComponentNode& clk_ref(*found_clk_ref->second);
  EXPECT_EQ(clk_ref.Value().identifier, "clk");
  EXPECT_EQ(clk_ref.Value().ref_type, ReferenceType::kObjectMember);
  EXPECT_EQ(clk_ref.Value().resolved_symbol, nullptr);  // not yet resolved

  const auto found_q_ref = port_refs.find("q");
  ASSERT_NE(found_q_ref, port_refs.end());
  const ReferenceComponentNode& q_ref(*found_q_ref->second);
  EXPECT_EQ(q_ref.Value().identifier, "q");
  EXPECT_EQ(q_ref.Value().ref_type, ReferenceType::kObjectMember);
  EXPECT_EQ(q_ref.Value().resolved_symbol, nullptr);  // not yet resolved

  // Get the local symbol definitions for wires "c" and "d".
  MUST_ASSIGN_LOOKUP_SYMBOL(c_node, rr_node, "c");
  MUST_ASSIGN_LOOKUP_SYMBOL(d_node, rr_node, "d");

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());

    // Expect to resolve local references to wires c and d
    EXPECT_EQ(c_ref->LastLeaf()->Value().resolved_symbol, &c_node);
    EXPECT_EQ(d_ref->LastLeaf()->Value().resolved_symbol, &d_node);

    // Expect to resolved named port references to "clk" and "q".
    EXPECT_EQ(clk_ref.Value().resolved_symbol, &clk_node);
    EXPECT_EQ(q_ref.Value().resolved_symbol, &q_node);
  }
}

TEST(BuildSymbolTableTest, ModuleInstanceNamedPortConnectionNonexistentPort) {
  TestVerilogSourceFile src("foobar.sv",
                            "module m (\n"
                            "  input wire clk,\n"
                            "  output reg q\n"
                            ");\n"
                            "endmodule\n"
                            "module rr;\n"
                            "  wire c;\n"
                            "  m m_inst(.clk(c), .p(c));"
                            // one type reference, two local net references
                            // two named port references, "p" does not exist
                            "endmodule\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(m_node, root_symbol, "m");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  MUST_ASSIGN_LOOKUP_SYMBOL(clk_node, m_node, "clk");

  MUST_ASSIGN_LOOKUP_SYMBOL(q_node, m_node, "q");

  MUST_ASSIGN_LOOKUP_SYMBOL(rr_node, root_symbol, "rr");

  const DependentReferences* m_inst_ref = nullptr;
  for (const auto& ref : rr_node_info.local_references_to_bind) {
    const absl::string_view base_ref(ref.components->Value().identifier);
    if (base_ref == "m_inst") m_inst_ref = &ref;  // instance self-reference
  }
  ASSERT_NE(m_inst_ref, nullptr);

  const ReferenceComponentNode& m_inst_ref_root(*m_inst_ref->components);
  ASSERT_EQ(m_inst_ref_root.Children().size(), 2);
  const ReferenceComponentMap port_refs(
      ReferenceComponentNodeMapView(m_inst_ref_root));

  const auto found_clk_ref = port_refs.find("clk");
  ASSERT_NE(found_clk_ref, port_refs.end());
  const ReferenceComponentNode& clk_ref(*found_clk_ref->second);
  EXPECT_EQ(clk_ref.Value().identifier, "clk");
  EXPECT_EQ(clk_ref.Value().ref_type, ReferenceType::kObjectMember);
  EXPECT_EQ(clk_ref.Value().resolved_symbol, nullptr);  // not yet resolved

  const auto found_p_ref = port_refs.find("p");
  ASSERT_NE(found_p_ref, port_refs.end());
  const ReferenceComponentNode& p_ref(*found_p_ref->second);
  EXPECT_EQ(p_ref.Value().identifier, "p");
  EXPECT_EQ(p_ref.Value().ref_type, ReferenceType::kObjectMember);
  EXPECT_EQ(p_ref.Value().resolved_symbol, nullptr);  // not yet resolved

  // Get the local symbol definitions for wire "c".
  MUST_ASSIGN_LOOKUP_SYMBOL(c_node, rr_node, "c");

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    ASSERT_EQ(resolve_diagnostics.size(), 1);
    const absl::Status& err(resolve_diagnostics.front());
    EXPECT_EQ(err.code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(err.message(),
                HasSubstr("No member symbol \"p\" in parent scope m."));

    // Expect to resolved named port reference to "clk", but not "p".
    EXPECT_EQ(clk_ref.Value().resolved_symbol, &clk_node);
    EXPECT_EQ(p_ref.Value().resolved_symbol, nullptr);  // failed to resolve
  }
}

TEST(BuildSymbolTableTest, OneGlobalIntParameter) {
  TestVerilogSourceFile src("foobar.sv", "localparam int mint = 1;\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint_param, root_symbol, "mint");
  EXPECT_EQ(mint_param_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_param_info.file_origin, &src);
  ASSERT_NE(mint_param_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(
      verible::StringSpanOfSymbol(*mint_param_info.declared_type.syntax_origin),
      "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, OneGlobalUndefinedTypeParameter) {
  TestVerilogSourceFile src("foobar.sv", "localparam foo_t gun = 1;\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(gun_param, root_symbol, "gun");
  EXPECT_EQ(gun_param_info.type, SymbolType::kParameter);
  EXPECT_EQ(gun_param_info.file_origin, &src);
  ASSERT_NE(gun_param_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(
      verible::StringSpanOfSymbol(*gun_param_info.declared_type.syntax_origin),
      "foo_t");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_EQ(resolve_diagnostics.size(), 1);
    const auto& err_status(resolve_diagnostics.front());
    EXPECT_EQ(err_status.code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(err_status.message(),
                HasSubstr("Unable to resolve symbol \"foo_t\""));
    EXPECT_EQ(
        gun_param_info.declared_type.user_defined_type->Value().resolved_symbol,
        nullptr);  // not resolved
  }
}

TEST(BuildSymbolTableTest, ReferenceOneParameterExpression) {
  TestVerilogSourceFile src("foobar.sv",
                            "localparam int mint = 1;\n"
                            "localparam int tea = mint;\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(tea, root_symbol, "tea");
  EXPECT_EQ(tea_info.type, SymbolType::kParameter);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint, root_symbol, "mint");
  EXPECT_EQ(mint_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_info.file_origin, &src);
  ASSERT_NE(mint_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(verible::StringSpanOfSymbol(*mint_info.declared_type.syntax_origin),
            "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  // There should be one reference: "mint" (line 2)
  EXPECT_EQ(root_symbol.Value().local_references_to_bind.size(), 1);
  const DependentReferences& ref(
      root_symbol.Value().local_references_to_bind.front());
  const ReferenceComponent& ref_comp(ref.components->Value());
  EXPECT_TRUE(ref.components->is_leaf());
  EXPECT_EQ(ref_comp.identifier, "mint");
  EXPECT_EQ(ref_comp.ref_type, ReferenceType::kUnqualified);
  EXPECT_EQ(ref_comp.resolved_symbol,
            nullptr);  // have not tried to resolve yet

  {  // resolve symbols
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());
    EXPECT_EQ(ref_comp.resolved_symbol, &mint);  // resolved
  }
}

TEST(BuildSymbolTableTest, OneUnresolvedReferenceInExpression) {
  TestVerilogSourceFile src("foobar.sv", "localparam int mint = spice;\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint, root_symbol, "mint");
  EXPECT_EQ(mint_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_info.file_origin, &src);
  ASSERT_NE(mint_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(verible::StringSpanOfSymbol(*mint_info.declared_type.syntax_origin),
            "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  // There should be one reference: "spice" (line 2)
  EXPECT_EQ(root_symbol.Value().local_references_to_bind.size(), 1);
  const DependentReferences& ref(
      root_symbol.Value().local_references_to_bind.front());
  const ReferenceComponent& ref_comp(ref.components->Value());
  EXPECT_TRUE(ref.components->is_leaf());
  EXPECT_EQ(ref_comp.identifier, "spice");
  EXPECT_EQ(ref_comp.ref_type, ReferenceType::kUnqualified);
  EXPECT_EQ(ref_comp.resolved_symbol,
            nullptr);  // have not tried to resolve yet

  {  // resolve symbols
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    ASSERT_EQ(resolve_diagnostics.size(), 1);
    const absl::Status& err_status(resolve_diagnostics.front());
    EXPECT_EQ(err_status.code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(err_status.message(),
                HasSubstr("Unable to resolve symbol \"spice\""));
    EXPECT_EQ(ref_comp.resolved_symbol, nullptr);  // still resolved
  }
}

TEST(BuildSymbolTableTest, PackageDeclarationSingle) {
  TestVerilogSourceFile src("foobar.sv", "package my_pkg;\nendpackage\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(my_pkg, root_symbol, "my_pkg");
  EXPECT_EQ(my_pkg_info.type, SymbolType::kPackage);
  EXPECT_EQ(my_pkg_info.file_origin, &src);
  EXPECT_EQ(my_pkg_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

TEST(BuildSymbolTableTest, ReferenceOneParameterFromPackageToRoot) {
  TestVerilogSourceFile src("foobar.sv",
                            "localparam int mint = 1;\n"
                            "package p;\n"
                            "localparam int tea = mint;\n"  // reference
                            "endpackage\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(p_pkg, root_symbol, "p");
  EXPECT_EQ(p_pkg_info.type, SymbolType::kPackage);

  ASSERT_EQ(p_pkg_info.local_references_to_bind.size(), 1);
  const ReferenceComponent& mint_ref(
      p_pkg_info.local_references_to_bind.front().components->Value());
  EXPECT_EQ(mint_ref.identifier, "mint");
  EXPECT_EQ(mint_ref.resolved_symbol, nullptr);  // not yet resolved

  MUST_ASSIGN_LOOKUP_SYMBOL(tea, p_pkg, "tea");  // p::tea
  EXPECT_EQ(tea_info.type, SymbolType::kParameter);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint, root_symbol, "mint");
  EXPECT_EQ(mint_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_info.file_origin, &src);
  ASSERT_NE(mint_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(verible::StringSpanOfSymbol(*mint_info.declared_type.syntax_origin),
            "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {  // resolve symbols
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());
    EXPECT_EQ(mint_ref.resolved_symbol, &mint);  // resolved "mint"
  }
}

TEST(BuildSymbolTableTest, ReferenceOneParameterFromRootToPackage) {
  TestVerilogSourceFile src(
      "foobar.sv",
      "package p;\n"
      "localparam int mint = 1;\n"
      "endpackage\n"
      "localparam int tea = p::mint;\n"  // qualified reference
  );
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(p_pkg, root_symbol, "p");
  EXPECT_EQ(p_pkg_info.type, SymbolType::kPackage);

  ASSERT_EQ(root_symbol.Value().local_references_to_bind.size(), 1);
  // p_mint_ref is the reference chain for "p::mint".
  const DependentReferences& p_mint_ref(
      root_symbol.Value().local_references_to_bind.front());
  const ReferenceComponent& p_ref(p_mint_ref.components->Value());
  EXPECT_EQ(p_ref.identifier, "p");
  EXPECT_EQ(p_ref.resolved_symbol, nullptr);  // not yet resolved
  const ReferenceComponent& mint_ref(p_mint_ref.LastLeaf()->Value());
  EXPECT_EQ(mint_ref.identifier, "mint");
  EXPECT_EQ(mint_ref.resolved_symbol, nullptr);  // not yet resolved

  MUST_ASSIGN_LOOKUP_SYMBOL(tea, root_symbol, "tea");
  EXPECT_EQ(tea_info.type, SymbolType::kParameter);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint, p_pkg, "mint");  // p::mint
  EXPECT_EQ(mint_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_info.file_origin, &src);
  ASSERT_NE(mint_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(verible::StringSpanOfSymbol(*mint_info.declared_type.syntax_origin),
            "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {  // resolve symbols
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_TRUE(resolve_diagnostics.empty());
    EXPECT_EQ(p_ref.resolved_symbol, &p_pkg);    // resolved "p"
    EXPECT_EQ(mint_ref.resolved_symbol, &mint);  // resolved "p::mint"
  }
}

TEST(BuildSymbolTableTest, ReferenceOneParameterFromRootToPackageNoSuchMember) {
  TestVerilogSourceFile src("foobar.sv",
                            "package p;\n"
                            "localparam int mint = 1;\n"
                            "endpackage\n"
                            "localparam int tea = p::zzz;\n"  // expect fail
  );
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(p_pkg, root_symbol, "p");
  EXPECT_EQ(p_pkg_info.type, SymbolType::kPackage);

  ASSERT_EQ(root_symbol.Value().local_references_to_bind.size(), 1);
  // p_mint_ref is the reference chain for "p::mint".
  const DependentReferences& p_mint_ref(
      root_symbol.Value().local_references_to_bind.front());
  const ReferenceComponent& p_ref(p_mint_ref.components->Value());
  EXPECT_EQ(p_ref.identifier, "p");
  EXPECT_EQ(p_ref.resolved_symbol, nullptr);  // not yet resolved
  const ReferenceComponent& zzz_ref(p_mint_ref.LastLeaf()->Value());
  EXPECT_EQ(zzz_ref.identifier, "zzz");
  EXPECT_EQ(zzz_ref.resolved_symbol, nullptr);  // not yet resolved

  MUST_ASSIGN_LOOKUP_SYMBOL(tea, root_symbol, "tea");
  EXPECT_EQ(tea_info.type, SymbolType::kParameter);

  MUST_ASSIGN_LOOKUP_SYMBOL(mint, p_pkg, "mint");
  EXPECT_EQ(mint_info.type, SymbolType::kParameter);
  EXPECT_EQ(mint_info.file_origin, &src);
  ASSERT_NE(mint_info.declared_type.syntax_origin, nullptr);
  EXPECT_EQ(verible::StringSpanOfSymbol(*mint_info.declared_type.syntax_origin),
            "int");
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  // resolving twice should not change results
  for (int i = 0; i < 2; ++i) {  // resolve symbols
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);
    EXPECT_EQ(resolve_diagnostics.size(), 1);
    EXPECT_EQ(p_ref.resolved_symbol, &p_pkg);     // resolved "p"
    EXPECT_EQ(zzz_ref.resolved_symbol, nullptr);  // unresolved "p::zzz"
  }
}

TEST(BuildSymbolTableTest, ClassDeclarationSingle) {
  TestVerilogSourceFile src("foobar.sv", "class ccc;\nendclass\n");
  const auto status = src.Parse();
  ASSERT_TRUE(status.ok()) << status.message();
  SymbolTable symbol_table(nullptr);
  const SymbolTableNode& root_symbol(symbol_table.Root());

  const auto build_diagnostics = BuildSymbolTable(src, &symbol_table);

  MUST_ASSIGN_LOOKUP_SYMBOL(ccc, root_symbol, "ccc");
  EXPECT_EQ(ccc_info.type, SymbolType::kClass);
  EXPECT_EQ(ccc_info.file_origin, &src);
  EXPECT_EQ(ccc_info.declared_type.syntax_origin,
            nullptr);  // there is no module meta-type
  EXPECT_TRUE(build_diagnostics.empty()) << "Unexpected diagnostic:\n"
                                         << build_diagnostics.front().message();

  {
    std::vector<absl::Status> resolve_diagnostics;
    symbol_table.Resolve(&resolve_diagnostics);  // nothing to resolve
    EXPECT_TRUE(resolve_diagnostics.empty());
  }
}

// TODO:
// expressions in ranges of dimensions.
// parameters package/module/class.
// generally, more testing unresolved symbols.

}  // namespace
}  // namespace verilog
