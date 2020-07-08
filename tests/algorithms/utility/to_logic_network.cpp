/*--------------------------------------------------------------------------------------------------
| This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
*-------------------------------------------------------------------------------------------------*/
#include "tweedledum/algorithms/utility/to_logic_network.hpp"

#include "tweedledum/networks/netlist.hpp"
#include "tweedledum/networks/op_dag.hpp"
#include "tweedledum/networks/wire.hpp"
#include "tweedledum/operations/w3_op.hpp"
#include "tweedledum/operations/wn32_op.hpp"

#include <catch.hpp>
#include <kitty/constructors.hpp>
#include <kitty/dynamic_truth_table.hpp>
#include <kitty/static_truth_table.hpp>
#include <mockturtle/algorithms/simulation.hpp>
#include <mockturtle/networks/xag.hpp>

using namespace mockturtle;
using namespace tweedledum;

TEMPLATE_PRODUCT_TEST_CASE("Conver simple quantum circuit to XAG", "[to_logic_network][template]",
                           (op_dag, netlist), (w3_op, wn32_op))
{
	TestType quantum_ntk;
	wire::id q0 = quantum_ntk.create_qubit("__i_0", wire::modes::in);
	wire::id q1 = quantum_ntk.create_qubit("__i_1", wire::modes::in);
	wire::id q2 = quantum_ntk.create_qubit("__o_0", wire::modes::out);

	quantum_ntk.create_op(gate_lib::ncx, std::vector<wire::id>({q0, q1}), std::vector<wire::id>({q2}));

	const auto logic_ntk = to_logic_network<xag_network>(quantum_ntk);

	kitty::dynamic_truth_table function(2u);
	kitty::create_from_binary_string(function, "1000");
	default_simulator<kitty::dynamic_truth_table> sim(function.num_vars());
	CHECK(simulate<kitty::dynamic_truth_table>(logic_ntk, sim)[0] == function);
}