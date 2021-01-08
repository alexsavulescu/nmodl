/*************************************************************************
 * Copyright (C) 2018-2020 Blue Brain Project
 *
 * This file is part of NMODL distributed under the terms of the GNU
 * Lesser General Public License. See top-level LICENSE file for details.
 *************************************************************************/

#include "codegen/llvm/codegen_llvm_visitor.hpp"
#include "codegen/llvm/codegen_llvm_helper_visitor.hpp"

#include "ast/all.hpp"
#include "codegen/codegen_helper_visitor.hpp"
#include "visitors/rename_visitor.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueSymbolTable.h"

namespace nmodl {
namespace codegen {


/****************************************************************************************/
/*                            Helper routines                                           */
/****************************************************************************************/

bool CodegenLLVMVisitor::check_array_bounds(const ast::IndexedName& node, unsigned index) {
    llvm::Type* array_type =
        local_named_values->lookup(node.get_node_name())->getType()->getPointerElementType();
    unsigned length = array_type->getArrayNumElements();
    return 0 <= index && index < length;
}

llvm::Value* CodegenLLVMVisitor::create_gep(const std::string& name, unsigned index) {
    llvm::Type* index_type = llvm::Type::getInt32Ty(*context);
    std::vector<llvm::Value*> indices;
    indices.push_back(llvm::ConstantInt::get(index_type, 0));
    indices.push_back(llvm::ConstantInt::get(index_type, index));

    return builder.CreateInBoundsGEP(local_named_values->lookup(name), indices);
}

llvm::Value* CodegenLLVMVisitor::codegen_indexed_name(const ast::IndexedName& node) {
    unsigned index = get_array_index_or_length(node);

    // Check if index is within array bounds.
    if (!check_array_bounds(node, index))
        throw std::runtime_error("Error: Index is out of bounds");

    return create_gep(node.get_node_name(), index);
}

unsigned CodegenLLVMVisitor::get_array_index_or_length(const ast::IndexedName& indexed_name) {
    auto integer = std::dynamic_pointer_cast<ast::Integer>(indexed_name.get_length());
    if (!integer)
        throw std::runtime_error("Error: expecting integer index or length");

    // Check if integer value is taken from a macro.
    if (!integer->get_macro())
        return integer->get_value();
    const auto& macro = sym_tab->lookup(integer->get_macro()->get_node_name());
    return static_cast<unsigned>(*macro->get_value());
}

void CodegenLLVMVisitor::run_llvm_opt_passes() {
    /// run some common optimisation passes that are commonly suggested
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass());

    /// initialize pass manager
    fpm.doInitialization();

    /// iterate over all functions and run the optimisation passes
    auto& functions = module->getFunctionList();
    for (auto& function: functions) {
        llvm::verifyFunction(function);
        fpm.run(function);
    }
}

void CodegenLLVMVisitor::create_external_method_call(const std::string& name,
                                                     const ast::ExpressionVector& arguments) {
    std::vector<llvm::Value*> argument_values;
    std::vector<llvm::Type*> argument_types;
    for (const auto& arg: arguments) {
        arg->accept(*this);
        llvm::Value* value = values.back();
        llvm::Type* type = value->getType();
        values.pop_back();
        argument_types.push_back(type);
        argument_values.push_back(value);
    }

#define DISPATCH(method_name, intrinsic)                                                           \
    if (name == method_name) {                                                                     \
        llvm::Value* result = builder.CreateIntrinsic(intrinsic, argument_types, argument_values); \
        values.push_back(result);                                                                  \
        return;                                                                                    \
    }

    DISPATCH("exp", llvm::Intrinsic::exp);
    DISPATCH("pow", llvm::Intrinsic::pow);
#undef DISPATCH

    throw std::runtime_error("Error: External method" + name + " is not currently supported");
}

void CodegenLLVMVisitor::create_function_call(llvm::Function* func,
                                              const std::string& name,
                                              const ast::ExpressionVector& arguments) {
    // Check that function is called with the expected number of arguments.
    if (arguments.size() != func->arg_size()) {
        throw std::runtime_error("Error: Incorrect number of arguments passed");
    }

    // Process each argument and add it to a vector to pass to the function call instruction. Note
    // that type checks are not needed here as NMODL operates on doubles by default.
    std::vector<llvm::Value*> argument_values;
    for (const auto& arg: arguments) {
        arg->accept(*this);
        llvm::Value* value = values.back();
        values.pop_back();
        argument_values.push_back(value);
    }

    llvm::Value* call = builder.CreateCall(func, argument_values);
    values.push_back(call);
}

void CodegenLLVMVisitor::emit_procedure_or_function_declaration(const ast::Block& node) {
    const auto& name = node.get_node_name();
    const auto& parameters = node.get_parameters();

    // Procedure or function parameters are doubles by default.
    std::vector<llvm::Type*> arg_types;
    for (size_t i = 0; i < parameters.size(); ++i)
        arg_types.push_back(llvm::Type::getDoubleTy(*context));

    // If visiting a function, the return type is a double by default.
    llvm::Type* return_type = node.is_function_block() ? llvm::Type::getDoubleTy(*context)
                                                       : llvm::Type::getVoidTy(*context);

    // Create a function that is automatically inserted into module's symbol table.
    llvm::Function::Create(llvm::FunctionType::get(return_type, arg_types, /*isVarArg=*/false),
                           llvm::Function::ExternalLinkage,
                           name,
                           *module);
}

void CodegenLLVMVisitor::visit_procedure_or_function(const ast::Block& node) {
    const auto& name = node.get_node_name();
    const auto& parameters = node.get_parameters();
    llvm::Function* func = module->getFunction(name);

    // Create the entry basic block of the function/procedure and point the local named values table
    // to the symbol table.
    llvm::BasicBlock* body = llvm::BasicBlock::Create(*context, /*Name=*/"", func);
    builder.SetInsertPoint(body);
    local_named_values = func->getValueSymbolTable();

    // When processing a function, it returns a value named <function_name> in NMODL. Therefore, we
    // first run RenameVisitor to rename it into ret_<function_name>. This will aid in avoiding
    // symbolic conflicts. Then, allocate the return variable on the local stack.
    std::string return_var_name = "ret_" + name;
    const auto& block = node.get_statement_block();
    if (node.is_function_block()) {
        visitor::RenameVisitor v(name, return_var_name);
        block->accept(v);
        builder.CreateAlloca(llvm::Type::getDoubleTy(*context),
                             /*ArraySize=*/nullptr,
                             return_var_name);
    }

    // Allocate parameters on the stack and add them to the symbol table.
    unsigned i = 0;
    for (auto& arg: func->args()) {
        std::string arg_name = parameters[i++].get()->get_node_name();
        llvm::Value* alloca = builder.CreateAlloca(arg.getType(), /*ArraySize=*/nullptr, arg_name);
        arg.setName(arg_name);
        builder.CreateStore(&arg, alloca);
    }

    // Process function or procedure body.
    block->accept(*this);

    // Add the terminator. If visiting function, we need to return the value specified by
    // ret_<function_name>.
    if (node.is_function_block()) {
        llvm::Value* return_var = builder.CreateLoad(local_named_values->lookup(return_var_name));
        builder.CreateRet(return_var);
    } else {
        builder.CreateRetVoid();
    }

    // Clear local values stack and remove the pointer to the local symbol table.
    values.clear();
    local_named_values = nullptr;
}


/****************************************************************************************/
/*                            Overloaded visitor routines                               */
/****************************************************************************************/


void CodegenLLVMVisitor::visit_binary_expression(const ast::BinaryExpression& node) {
    const auto& op = node.get_op().get_value();

    // Process rhs first, since lhs is handled differently for assignment and binary
    // operators.
    node.get_rhs()->accept(*this);
    llvm::Value* rhs = values.back();
    values.pop_back();
    if (op == ast::BinaryOp::BOP_ASSIGN) {
        auto var = dynamic_cast<ast::VarName*>(node.get_lhs().get());
        if (!var) {
            throw std::runtime_error("Error: only VarName assignment is currently supported.\n");
        }

        const auto& identifier = var->get_name();
        if (identifier->is_name()) {
            llvm::Value* alloca = local_named_values->lookup(var->get_node_name());
            builder.CreateStore(rhs, alloca);
        } else if (identifier->is_indexed_name()) {
            auto indexed_name = std::dynamic_pointer_cast<ast::IndexedName>(identifier);
            builder.CreateStore(rhs, codegen_indexed_name(*indexed_name));
        } else {
            throw std::runtime_error("Error: Unsupported variable type");
        }
        return;
    }

    node.get_lhs()->accept(*this);
    llvm::Value* lhs = values.back();
    values.pop_back();
    llvm::Value* result;

    // \todo: Support other binary operators
    switch (op) {
#define DISPATCH(binary_op, llvm_op) \
    case binary_op:                  \
        result = llvm_op(lhs, rhs);  \
        values.push_back(result);    \
        break;

        DISPATCH(ast::BinaryOp::BOP_ADDITION, builder.CreateFAdd);
        DISPATCH(ast::BinaryOp::BOP_DIVISION, builder.CreateFDiv);
        DISPATCH(ast::BinaryOp::BOP_MULTIPLICATION, builder.CreateFMul);
        DISPATCH(ast::BinaryOp::BOP_SUBTRACTION, builder.CreateFSub);

#undef DISPATCH
    }
}

void CodegenLLVMVisitor::visit_boolean(const ast::Boolean& node) {
    const auto& constant = llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context),
                                                  node.get_value());
    values.push_back(constant);
}

// Generating FOR loop in LLVM IR creates the following structure:
//
//  +---------------------------+
//  | <code before for loop>    |
//  | <for loop initialisation> |
//  | br %cond                  |
//  +---------------------------+
//                |
//                V
//  +-----------------------------+
//  | <condition code>            |
//  | %cond = ...                 |<------+
//  | cond_br %cond, %body, %exit |       |
//  +-----------------------------+       |
//      |                 |               |
//      |                 V               |
//      |     +------------------------+  |
//      |     | <body code>            |  |
//      |     | br %inc                |  |
//      |     +------------------------+  |
//      |                 |               |
//      |                 V               |
//      |     +------------------------+  |
//      |     | <increment code>       |  |
//      |      | br %cond              |  |
//      |     +------------------------+  |
//      |                 |               |
//      |                 +---------------+
//      V
//  +---------------------------+
//  | <code after for loop>     |
//  +---------------------------+
void CodegenLLVMVisitor::visit_codegen_for_statement(const ast::CodegenForStatement& node) {
    // First, initialise the loop in the same basic block.
    node.get_initialization()->accept(*this);

    // Branch to condition basic block and insert condition code there.
    llvm::BasicBlock* for_cond = llvm::BasicBlock::Create(*context, /*Name=*/"for.cond");
    builder.CreateBr(for_cond);
    builder.SetInsertPoint(for_cond);
    node.get_condition()->accept(*this);

    // Extract the condition to decide whether to branch to the loop body or loop exit.
    llvm::Value* cond = values.back();
    values.pop_back();
    llvm::BasicBlock* for_body = llvm::BasicBlock::Create(*context, /*Name=*/"for.body");
    llvm::BasicBlock* for_inc = llvm::BasicBlock::Create(*context, /*Name=*/"for.inc");
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(*context, /*Name=*/"for.exit");
    builder.CreateCondBr(cond, for_body, exit);

    // Generate code for the loop body and create the basic block for the increment.
    builder.SetInsertPoint(for_body);
    const auto& statement_block = node.get_statement_block();
    statement_block->accept(*this);
    builder.CreateBr(for_inc);

    // Process increment.
    builder.SetInsertPoint(for_inc);
    node.get_increment()->accept(*this);
    builder.CreateBr(for_cond);

    // Generate exit code out of the loop.
    builder.SetInsertPoint(exit);
}

void CodegenLLVMVisitor::visit_double(const ast::Double& node) {
    const auto& constant = llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context),
                                                 node.get_value());
    values.push_back(constant);
}

void CodegenLLVMVisitor::visit_function_block(const ast::FunctionBlock& node) {
    visit_procedure_or_function(node);
}

void CodegenLLVMVisitor::visit_function_call(const ast::FunctionCall& node) {
    const auto& name = node.get_node_name();
    auto func = module->getFunction(name);
    if (func) {
        create_function_call(func, name, node.get_arguments());
    } else {
        auto symbol = sym_tab->lookup(name);
        if (symbol && symbol->has_any_property(symtab::syminfo::NmodlType::extern_method)) {
            create_external_method_call(name, node.get_arguments());
        } else {
            throw std::runtime_error("Error: Unknown function name: " + name +
                                     ". (External functions references are not supported)");
        }
    }
}

void CodegenLLVMVisitor::visit_integer(const ast::Integer& node) {
    const auto& constant = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context),
                                                  node.get_value());
    values.push_back(constant);
}

void CodegenLLVMVisitor::visit_local_list_statement(const ast::LocalListStatement& node) {
    for (const auto& variable: node.get_variables()) {
        std::string name = variable->get_node_name();
        const auto& identifier = variable->get_name();
        // Local variable can be a scalar (Node AST class) or an array (IndexedName AST class). For
        // each case, create memory allocations with the corresponding LLVM type.
        llvm::Type* var_type;
        if (identifier->is_indexed_name()) {
            auto indexed_name = std::dynamic_pointer_cast<ast::IndexedName>(identifier);
            unsigned length = get_array_index_or_length(*indexed_name);
            var_type = llvm::ArrayType::get(llvm::Type::getDoubleTy(*context), length);
        } else if (identifier->is_name()) {
            // This case corresponds to a scalar local variable. Its type is double by default.
            var_type = llvm::Type::getDoubleTy(*context);
        } else {
            throw std::runtime_error("Error: Unsupported local variable type");
        }
        builder.CreateAlloca(var_type, /*ArraySize=*/nullptr, name);
    }
}

void CodegenLLVMVisitor::visit_program(const ast::Program& node) {
    // Before generating LLVM, gather information about AST. For now, information about functions
    // and procedures is used only.
    CodegenHelperVisitor v;
    CodegenInfo info = v.analyze(node);

    // For every function and procedure, generate its declaration. Thus, we can look up
    // `llvm::Function` in the symbol table in the module.
    for (const auto& func: info.functions) {
        emit_procedure_or_function_declaration(*func);
    }
    for (const auto& proc: info.procedures) {
        emit_procedure_or_function_declaration(*proc);
    }

    // Set the AST symbol table.
    sym_tab = node.get_symbol_table();

    // Proceed with code generation.
    node.visit_children(*this);

    if (opt_passes) {
        logger->info("Running LLVM optimisation passes");
        run_llvm_opt_passes();
    }

    // Keep this for easier development (maybe move to debug mode later).
    std::cout << print_module();

    // not used yet : this will be used at the beginning of this function
    {
        CodegenLLVMHelperVisitor v;
        v.visit_program(const_cast<ast::Program&>(node));
    }
}

void CodegenLLVMVisitor::visit_procedure_block(const ast::ProcedureBlock& node) {
    visit_procedure_or_function(node);
}

void CodegenLLVMVisitor::visit_statement_block(const ast::StatementBlock& node) {
    const auto& statements = node.get_statements();
    for (const auto& statement: statements) {
        // \todo: Support other statement types.
        if (statement->is_local_list_statement() || statement->is_expression_statement())
            statement->accept(*this);
    }
}

void CodegenLLVMVisitor::visit_unary_expression(const ast::UnaryExpression& node) {
    ast::UnaryOp op = node.get_op().get_value();
    node.get_expression()->accept(*this);
    llvm::Value* value = values.back();
    values.pop_back();
    if (op == ast::UOP_NEGATION) {
        llvm::Value* result = builder.CreateFNeg(value);
        values.push_back(result);
    } else {
        // Support only `double` operators for now.
        throw std::runtime_error("Error: unsupported unary operator\n");
    }
}

void CodegenLLVMVisitor::visit_var_name(const ast::VarName& node) {
    const auto& identifier = node.get_name();
    if (!identifier->is_name() && !identifier->is_indexed_name())
        throw std::runtime_error("Error: Unsupported variable type");

    llvm::Value* ptr;
    if (identifier->is_name())
        ptr = local_named_values->lookup(node.get_node_name());

    if (identifier->is_indexed_name()) {
        auto indexed_name = std::dynamic_pointer_cast<ast::IndexedName>(identifier);
        ptr = codegen_indexed_name(*indexed_name);
    }

    // Finally, load the variable from the pointer value.
    llvm::Value* var = builder.CreateLoad(ptr);
    values.push_back(var);
}

}  // namespace codegen
}  // namespace nmodl
