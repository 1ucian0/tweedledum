/*------------------------------------------------------------------------------
| Part of tweedledum.  This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
*-----------------------------------------------------------------------------*/
#pragma once

#include "../../ir/Circuit.h"
#include "../../ir/GateLib.h"
#include "../../ir/Wire.h"

#include <array>
#include <cassert>
#include <mockturtle/networks/xag.hpp>
#include <mockturtle/utils/node_map.hpp>
#include <vector>

namespace tweedledum {
#pragma region Implementation details
namespace xag_synth_detail {

struct ParityAnd {
	std::vector<uint32_t> in0;
	std::vector<uint32_t> in1;
	std::vector<uint32_t> in01;
	std::array<bool, 2> is_complemented;
	uint32_t ref_count : 30;
	uint32_t is_and : 1;
	uint32_t in_output : 1;
	uint32_t cleanup;
	uint32_t n_ands;

	ParityAnd() : ref_count(0), is_and(0), cleanup(0), n_ands(0) {}
};

enum class Action : uint8_t {
	compute,
	cleanup,
};

struct Step {
	Action action;
	uint32_t node;
	Step(Action a, uint32_t n) : action(a), node(n) {}
};

std::vector<ParityAnd> collapse_xag(mockturtle::xag_network const& xag)
{
	using XAG = mockturtle::xag_network;
	using Node = typename XAG::node;
	using Signal = typename XAG::signal;
	using LTFI = std::vector<Signal>;

	// Preprocess:
	// - Compute LTFI
	mockturtle::node_map<LTFI, XAG> ltfi(xag);
	xag.foreach_pi([&](Node const& node, uint32_t index) {
		ltfi[node].emplace_back(xag.make_signal(node));
		xag.set_value(node, index + 1);
		return;
	});
	uint32_t num_and = 0;
	xag.foreach_gate([&](Node const& node) {
		std::array<LTFI const*, 2> node_ltfi;
		xag.foreach_fanin(node, [&](Signal const signal, uint32_t i) {
			node_ltfi[i] = &(ltfi[signal]);
		});
		if (xag.is_and(node)) {
			ltfi[node].emplace_back(xag.make_signal(node));
			xag.set_value(node, num_and + 1 + xag.num_pis());
			num_and += 1;
			return;
		}
		// The node is then a XOR
		std::set_symmetric_difference(node_ltfi[0]->cbegin(),
		    node_ltfi[0]->cend(), node_ltfi[1]->cbegin(),
		    node_ltfi[1]->cend(), std::back_inserter(ltfi[node]));
		assert(ltfi[node].size());
	});

	// Create the collapsed XAG
	std::vector<ParityAnd> gates;
	gates.reserve(1 + xag.num_pis() + num_and + xag.num_pos());
	xag.foreach_node([&](Node const& node) {
		// FIXME: seems like the constant node is considered a PI!!
		if (xag.is_pi(node)) {
			gates.emplace_back();
			return;
		}
		if (!xag.is_and(node)) {
			return;
		}
		auto& gate = gates.emplace_back();
		gate.ref_count = 0;
		gate.is_and = 1;
		gate.cleanup = 1;
		std::array<LTFI const*, 2> node_ltfi;
		xag.foreach_fanin(node, [&](Signal const signal, uint32_t i) {
			node_ltfi[i] = &(ltfi[signal]);
			gate.is_complemented[i] = xag.is_complemented(signal);
		});
		auto in0_begin = node_ltfi[0]->cbegin();
		auto in0_end = node_ltfi[0]->cend();
		auto in1_begin = node_ltfi[1]->cbegin();
		auto in1_end = node_ltfi[1]->cend();
		while (in0_begin != in0_end && in1_begin != in1_end) {
			if (*in0_begin == *in1_begin) {
				uint32_t id = xag.value(xag.get_node(*in0_begin));
				gate.in01.emplace_back(id);
				gates[id].ref_count += 1;
				++in0_begin;
				++in1_begin;
			} else if (*in0_begin < *in1_begin) {
				uint32_t id = xag.value(xag.get_node(*in0_begin));
				gate.in0.emplace_back(id);
				gates[id].ref_count += 1;
				++in0_begin;
			} else {
				uint32_t id = xag.value(xag.get_node(*in1_begin));
				gate.in1.emplace_back(id);
				gates[id].ref_count += 1;
				++in1_begin;
			}
		}
		while (in0_begin != in0_end) {
			uint32_t id = xag.value(xag.get_node(*in0_begin));
			gate.in0.emplace_back(id);
			gates[id].ref_count += 1;
			++in0_begin;
		}
		while (in1_begin != in1_end) {
			uint32_t id = xag.value(xag.get_node(*in1_begin));
			gate.in1.emplace_back(id);
			gates[id].ref_count += 1;
			++in1_begin;
		}
		if (gate.in0.size() < gate.in1.size()) {
			std::swap(gate.in0, gate.in1);
			std::swap(gate.is_complemented[0], gate.is_complemented[1]);
		}
	});
	xag.foreach_po([&](Signal const& signal) {
		auto& gate = gates.emplace_back();
		for (Signal const input : ltfi[signal]) {
			uint32_t id = xag.value(xag.get_node(input));
			gate.in0.emplace_back(id);
			gates[id].ref_count += 1;
			gates[id].in_output = 1;
			if (gates[id].is_and) {
				gate.n_ands += 1;
			}
		}
		gate.cleanup = 0;
		gate.is_complemented[0] = xag.is_complemented(signal);
	});
	return gates;
}

void pre_assign_qubits(mockturtle::xag_network const& xag,
    std::vector<WireRef> const& qubits, std::vector<ParityAnd>& gates,
    std::vector<WireRef>& to_qubit)
{
	uint32_t const output_begin = gates.size() - xag.num_pos();
	uint32_t qubit_idx = 0;
	// Assing qubits to inputs
	for (; qubit_idx < xag.num_pis(); ++qubit_idx) {
		to_qubit[qubit_idx + 1] = qubits.at(qubit_idx);
	}
	// Assing qubits to outputs and ParityAnd gates that are only referenced
	// once and by a output.
	for (uint32_t i = output_begin; i < gates.size(); ++i, ++qubit_idx) {
		for (uint32_t id : gates[i].in0) {
			if (!gates[id].is_and || gates[id].ref_count > 1) {
				continue;
			}
			to_qubit[i] = qubits[qubit_idx];
			to_qubit[id] = qubits[qubit_idx];
			gates[id].cleanup = 0;
			break;
		}
	}
}

void post_assign_qubits(mockturtle::xag_network const& xag,
    std::vector<WireRef> const& qubits, std::vector<ParityAnd>& gates,
    std::vector<WireRef>& to_qubit)
{
	uint32_t const output_begin = gates.size() - xag.num_pos();
	// Assing qubits to outputs that haven't been assigned and that have 
	// only one ParityAnd gate as input
	uint32_t qubit_idx = xag.num_pis();
	for (uint32_t i = output_begin; i < gates.size(); ++i, ++qubit_idx) {
		if (to_qubit[i] == qubits[qubit_idx] || gates[i].n_ands != 1) {
			continue;
		}
		for (uint32_t id : gates[i].in0) {
			assert(gates[id].ref_count);
			if (!gates[id].is_and || to_qubit[id] != WireRef::invalid()) {
				continue;
			}
			to_qubit[i] = qubits.at(qubit_idx);
			to_qubit[id] = to_qubit[i];
			gates[i].cleanup = 2;
			gates[id].cleanup = 0;
			break;
		}
	}
	qubit_idx = xag.num_pis();
	for (uint32_t i = output_begin; i < gates.size(); ++i, ++qubit_idx) {
		if (to_qubit[i] == qubits[qubit_idx]) {
			continue;
		}
		to_qubit[i] = qubits.at(qubit_idx);
		for (uint32_t id : gates[i].in0) {
			assert(gates[id].ref_count);
			if (!gates[id].is_and || to_qubit[id] != WireRef::invalid()) {
				continue;
			}
			if (gates[id].ref_count == 1) {
				to_qubit[id] = to_qubit[i];
				gates[id].cleanup = 0;
				break;
			}
		}
	}
}

void try_cleanup(std::vector<ParityAnd>& gates, ParityAnd& gate, std::vector<Step>& steps) 
{
	for (uint32_t id : gate.in0) {
		gates[id].ref_count -= 1;
		if (gates[id].ref_count == 0 && gates[id].is_and) {
			assert(gates[id].cleanup);
			steps.emplace_back(Action::cleanup, id);
			gates[id].cleanup = 0;
			try_cleanup(gates, gates[id], steps);
		}
	}
	for (uint32_t id : gate.in1) {
		gates[id].ref_count -= 1;
		if (gates[id].ref_count == 0 && gates[id].is_and) {
			assert(gates[id].cleanup);
			steps.emplace_back(Action::cleanup, id);
			gates[id].cleanup = 0;
			try_cleanup(gates, gates[id], steps);
		}
	}
	for (uint32_t id : gate.in01) {
		gates[id].ref_count -= 1;
		if (gates[id].ref_count == 0 && gates[id].is_and) {
			assert(gates[id].cleanup);
			steps.emplace_back(Action::cleanup, id);
			gates[id].cleanup = 0;
			try_cleanup(gates, gates[id], steps);
		}
	}
}

void compute_parity(Circuit& circuit, std::vector<WireRef> const& qubits)
{
	if (qubits.size() == 1) {
		return;
	}
	circuit.create_instruction(GateLib::Parity(), qubits);
}

void compute_gate(Circuit& circuit, ParityAnd const& gate,
    std::vector<WireRef> const& to_qubit, WireRef target)
{
	std::vector<WireRef> in0;
	std::vector<WireRef> in1;
	std::vector<WireRef> in01;
	for (uint32_t i : gate.in0) {
		in0.push_back(to_qubit.at(i));
	}
	for (uint32_t i : gate.in1) {
		in1.push_back(to_qubit.at(i));
	}
	for (uint32_t i : gate.in01) {
		in01.push_back(to_qubit.at(i));
	}
	// Compute the inputs to the Toffoli gate (inplace)
	compute_parity(circuit, in0);
	if (!in01.empty()) {
		compute_parity(circuit, in01);
		in1.push_back(in01.back());
		circuit.create_instruction(
		    GateLib::X(), {in01.back()}, in0.back());
	}
	compute_parity(circuit, in1);
	// Compute Toffoli
	WireRef c0 = gate.is_complemented[0] ? !in0.back() : in0.back();
	WireRef c1 = gate.is_complemented[1] ? !in1.back() : in1.back();
	circuit.create_instruction(GateLib::X(), {c0, c1}, target);
	// Cleanup the input to the Toffoli gate
	compute_parity(circuit, in1);
	if (!in01.empty()) {
		circuit.create_instruction(
		    GateLib::X(), {in01.back()}, in0.back());
		compute_parity(circuit, in01);
	}
	compute_parity(circuit, in0);
}

void execute_steps(std::vector<Step> const& steps,
    std::vector<ParityAnd> const& gates, std::vector<WireRef>& to_qubit,
    Circuit& circuit)
{
	for (Step const& step : steps) {
		ParityAnd const& gate = gates[step.node];
		switch (step.action) {
		case Action::compute:
			if (to_qubit[step.node] == WireRef::invalid()) {
				to_qubit[step.node] = circuit.request_ancilla();
			}
			compute_gate(
			    circuit, gate, to_qubit, to_qubit[step.node]);
			break;

		case Action::cleanup:
			circuit.release_ancilla(to_qubit[step.node]);
			compute_gate(
			    circuit, gate, to_qubit, to_qubit[step.node]);
			break;
		}
	}
}

void compute_outputs(mockturtle::xag_network const& xag,
    std::vector<ParityAnd> const& gates, std::vector<WireRef>& to_qubit,
    Circuit& circuit)
{
	uint32_t const output_begin = gates.size() - xag.num_pos();
	for (uint32_t i = output_begin; i < gates.size(); ++i) {
		if (gates[i].cleanup == 2) {
			continue;
		}
		assert(gates[i].cleanup == 0);
		std::vector<WireRef> qs;
		for (uint32_t id : gates[i].in0) {
			if (to_qubit[id] == to_qubit[i]) {
				continue;
			}
			qs.push_back(to_qubit[id]);
		}
		qs.push_back(to_qubit[i]);
		compute_parity(circuit, qs);
	}
	for (uint32_t i = output_begin; i < gates.size(); ++i) {
		if (gates[i].cleanup != 2) {
			continue;
		}
		assert(gates[i].cleanup == 2);
		std::vector<WireRef> qs;
		for (uint32_t id : gates[i].in0) {
			if (to_qubit[id] == to_qubit[i]) {
				continue;
			}
			qs.push_back(to_qubit[id]);
		}
		qs.push_back(to_qubit[i]);
		compute_parity(circuit, qs);
	}
	for (uint32_t i = output_begin; i < gates.size(); ++i) {
		if (gates[i].is_complemented[0]) {
			circuit.create_instruction(GateLib::X(), {to_qubit[i]});
		}
	}
}

void synthesize(Circuit& circuit, std::vector<WireRef> const& qubits,
    mockturtle::xag_network const& xag)
{
	std::vector<ParityAnd> gates = collapse_xag(xag);
	std::vector<WireRef> to_qubit(gates.size(), WireRef::invalid());

	pre_assign_qubits(xag, qubits, gates, to_qubit);

	// Compute steps 
	uint32_t const gates_begin = xag.num_pis() + 1;
	uint32_t const output_begin = gates.size() - xag.num_pos();
	std::vector<Step> steps;
	for (uint32_t i = gates_begin; i < output_begin; ++i) {
		steps.emplace_back(Action::compute, i);
		// Eagerly try to cleanup
		if (gates[i].in_output) {
			try_cleanup(gates, gates[i], steps);
		}
	}

	post_assign_qubits(xag, qubits, gates, to_qubit);
	execute_steps(steps, gates, to_qubit, circuit);
	compute_outputs(xag, gates, to_qubit, circuit);
	
	// Clean up whats is left
	std::for_each(steps.crbegin(), steps.crend(), [&](Step const& step) {
		ParityAnd& gate = gates[step.node];
		if (gate.cleanup == 0) {
			return;
		}
		assert(gate.cleanup == 1);
		circuit.release_ancilla(to_qubit[step.node]);
		compute_gate(circuit, gate, to_qubit, to_qubit[step.node]);
	});
}

} // namespace xag_synth_detail
#pragma endregion

inline void xag_synth(Circuit& circuit, std::vector<WireRef> const& qubits,
    mockturtle::xag_network const& xag)
{
	xag_synth_detail::synthesize(circuit, qubits, xag);
}

inline Circuit xag_synth(mockturtle::xag_network const& xag)
{
	// TODO: method to generate a name;
	Circuit circuit("my_circuit");
	// Create the necessary qubits
	uint32_t const nun_qubits = xag.num_pis() + xag.num_pos();
	std::vector<WireRef> wires;
	wires.reserve(nun_qubits);
	for (uint32_t i = 0u; i < nun_qubits; ++i) {
		wires.emplace_back(circuit.create_qubit());
	}
	xag_synth(circuit, wires, xag);
	return circuit;
}

} // namespace tweedledum