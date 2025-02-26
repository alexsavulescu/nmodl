/*************************************************************************
 * Copyright (C) 2018-2019 Blue Brain Project
 *
 * This file is part of NMODL distributed under the terms of the GNU
 * Lesser General Public License. See top-level LICENSE file for details.
 *************************************************************************/

#include "visitors/steadystate_visitor.hpp"

#include "ast/all.hpp"
#include "codegen/codegen_naming.hpp"
#include "utils/logger.hpp"
#include "visitors/visitor_utils.hpp"

namespace nmodl {
namespace visitor {

using namespace fmt::literals;
using symtab::syminfo::NmodlType;

std::shared_ptr<ast::DerivativeBlock> SteadystateVisitor::create_steadystate_block(
    const std::shared_ptr<ast::SolveBlock>& solve_block,
    const std::vector<std::shared_ptr<ast::Ast>>& deriv_blocks) {
    // new block to be returned
    std::shared_ptr<ast::DerivativeBlock> ss_block;

    // get method & derivative block
    const auto solve_block_name = solve_block->get_block_name()->get_value()->eval();
    const auto steadystate_method = solve_block->get_steadystate()->get_value()->eval();

    logger->debug("SteadystateVisitor :: Found STEADYSTATE SOLVE statement: using {} for {}",
                  steadystate_method,
                  solve_block_name);

    ast::DerivativeBlock* deriv_block_ptr = nullptr;
    for (const auto& block_ptr: deriv_blocks) {
        auto deriv_block = std::dynamic_pointer_cast<ast::DerivativeBlock>(block_ptr);
        if (deriv_block->get_node_name() == solve_block_name) {
            logger->debug("SteadystateVisitor :: -> found corresponding DERIVATIVE block: {}",
                          solve_block_name);
            deriv_block_ptr = deriv_block.get();
            break;
        }
    }

    if (deriv_block_ptr != nullptr) {
        // make a clone of derivative block with "_steadystate" suffix
        ss_block = std::shared_ptr<ast::DerivativeBlock>(deriv_block_ptr->clone());
        auto ss_name = ss_block->get_name();
        ss_name->set_name(ss_name->get_value()->get_value() + "_steadystate");
        auto ss_name_clone = std::shared_ptr<ast::Name>(ss_name->clone());
        ss_block->set_name(std::move(ss_name));
        logger->debug("SteadystateVisitor :: -> adding new DERIVATIVE block: {}",
                      ss_block->get_node_name());

        // create statements to alter value of dt within DERIVATIVE block
        // TODO: make sure dt_tmp_var_name variable name does not clash
        std::string dt_tmp_var_name = codegen::naming::NTHREAD_DT_VARIABLE + "_saved_value";
        std::string dt_save = dt_tmp_var_name + " = " + codegen::naming::NTHREAD_DT_VARIABLE;
        std::string dt_assign = codegen::naming::NTHREAD_DT_VARIABLE + " = ";
        std::string dt_restore = dt_assign + dt_tmp_var_name;
        if (steadystate_method == codegen::naming::SPARSE_METHOD) {
            dt_assign += "{:.16g}"_format(STEADYSTATE_SPARSE_DT);
        } else if (steadystate_method == codegen::naming::DERIVIMPLICIT_METHOD) {
            dt_assign += "{:.16g}"_format(STEADYSTATE_DERIVIMPLICIT_DT);
        } else {
            logger->warn("SteadystateVisitor :: solve method {} not supported for STEADYSTATE",
                         steadystate_method);
            return nullptr;
        }
        auto statement_block = ss_block->get_statement_block();
        // declare tmp variable to save dt value (this will go into the LOCAL statement at the top
        // of the statement block)
        add_local_variable(*statement_block.get(), dt_tmp_var_name);
        // get a copy of existing statements
        auto statements = statement_block->get_statements();
        // insert dt_save and dt_assign statements just below first LOCAL statement
        auto insertion_point = statements.begin();
        while ((*insertion_point)->is_local_list_statement()) {
            ++insertion_point;
        }
        insertion_point = statements.insert(insertion_point, create_statement(dt_save));
        ++insertion_point;
        statements.insert(insertion_point, create_statement(dt_assign));
        // insert dt_restore statement at the end
        statements.push_back(create_statement(dt_restore));
        // replace old set of statements in AST with new one
        statement_block->set_statements(std::move(statements));

        // update SOLVE statement:
        // set name to point to new DERIVATIVE block
        solve_block->set_block_name(std::move(ss_name_clone));
        // change from STEADYSTATE to METHOD
        solve_block->set_method(solve_block->get_steadystate());
        solve_block->set_steadystate(nullptr);
    } else {
        logger->warn("SteadystateVisitor :: Could not find derivative block {} for STEADYSTATE",
                     solve_block_name);
        return nullptr;
    }
    return ss_block;
}

void SteadystateVisitor::visit_program(ast::Program& node) {
    // get DERIVATIVE blocks
    const auto& deriv_blocks = collect_nodes(node, {ast::AstNodeType::DERIVATIVE_BLOCK});

    // get list of STEADYSTATE solve statements with names & methods
    const auto& solve_block_nodes = collect_nodes(node, {ast::AstNodeType::SOLVE_BLOCK});

    // create new DERIVATIVE blocks for the STEADYSTATE solves
    for (const auto& solve_block_ptr: solve_block_nodes) {
        if (auto solve_block = std::dynamic_pointer_cast<ast::SolveBlock>(solve_block_ptr)) {
            if (solve_block->get_steadystate()) {
                auto ss_block = create_steadystate_block(solve_block, deriv_blocks);
                if (ss_block != nullptr) {
                    node.emplace_back_node(ss_block);
                }
            }
        }
    }
}

}  // namespace visitor
}  // namespace nmodl
