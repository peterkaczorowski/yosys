/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct GowinFixportsPass : public Pass {
	GowinFixportsPass() : Pass("gowin_fixports", "prepare primitive and port connections for the vendor netlist reader") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    gowin_fixports [selection]\n");
		log("\n");
		log("Prepare a mapped design for the Gowin place and route tool, whose netlist\n");
		log("reader is stricter than Verilog:\n");
		log("\n");
		log("  - cell pins must connect to module port wires directly, not through an\n");
		log("    alias (assign); cell connections are moved onto the port wires and\n");
		log("    redundant aliases are removed with 'opt_clean -purge'.\n");
		log("\n");
		log("  - a differential output buffer (TLVDS/ELVDS) must drive the pads itself;\n");
		log("    an OBUF inserted behind it (as nextpnr expects) is removed.\n");
		log("\n");
		log("  - primitive instances need complete pin lists; every unconnected output\n");
		log("    port of an instance of a library (blackbox or whitebox) module is\n");
		log("    connected to new 1-bit dangling wires.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing GOWIN_FIXPORTS pass (prepare connections for the vendor netlist reader).\n");
		extra_args(args, 1, design);

		// iopadmap puts an OBUF behind a user-instantiated differential output
		// buffer, which nextpnr expects; the vendor tool wants the differential
		// buffer on the pads itself, so bypass and remove such an OBUF.
		int bypassed = 0;
		for (auto module : design->selected_modules()) {
			SigMap sigmap(module);
			pool<SigBit> diff_outputs;
			// Vendor cell names are not static IDs, so compare them as strings
			for (auto cell : module->selected_cells()) {
				std::string type = cell->type.unescape();
				if (type != "TLVDS_OBUF" && type != "ELVDS_OBUF" && type != "TLVDS_TBUF" && type != "ELVDS_TBUF")
					continue;
				for (auto &conn : cell->connections()) {
					std::string port = module->twines().str(conn.first);
					if (port == "\\O" || port == "\\OB")
						for (auto bit : sigmap(conn.second))
							diff_outputs.insert(bit);
				}
			}

			std::vector<RTLIL::Cell*> obufs;
			for (auto cell : module->selected_cells())
				if (cell->type.unescape() == "OBUF" && cell->hasPort(ID::I) && cell->hasPort(ID::O) &&
						diff_outputs.count(sigmap(cell->getPort(ID::I).as_bit())))
					obufs.push_back(cell);
			for (auto cell : obufs) {
				module->connect(cell->getPort(ID::O), cell->getPort(ID::I));
				module->remove(cell);
				bypassed++;
			}
		}
		log("Bypassed %d output buffers behind differential buffers.\n", bypassed);

		int moved = 0;
		for (auto module : design->selected_modules()) {
			SigMap sigmap(module);
			// A port bit becomes the representative of its net, unless the net
			// is a constant or already represented by another port bit
			for (auto wire : module->wires())
				if (wire->port_id)
					for (auto bit : SigSpec(wire)) {
						SigBit rep = sigmap(bit);
						if (rep.wire == nullptr || (rep.wire->port_id && rep != bit))
							continue;
						sigmap.database.promote(bit);
					}
			for (auto cell : module->selected_cells()) {
				dict<IdString, SigSpec> updates;
				for (auto &conn : cell->connections()) {
					SigSpec sig = conn.second;
					bool changed = false;
					for (auto &bit : sig) {
						SigBit rep = sigmap(bit);
						if (rep != bit && rep.wire != nullptr && rep.wire->port_id) {
							bit = rep;
							changed = true;
						}
					}
					if (changed)
						updates[conn.first] = sig;
				}
				for (auto &it : updates) {
					cell->setPort(it.first, it.second);
					moved++;
				}
			}
		}
		log("Moved %d cell connections onto port wires.\n", moved);
		Pass::call(design, "opt_clean -purge");

		int count = 0;
		for (auto module : design->selected_modules())
			for (auto cell : module->selected_cells()) {
				RTLIL::Module *lib = design->module(cell->type);
				if (lib == nullptr || !lib->get_blackbox_attribute(/*ignore_wb=*/false))
					continue;
				for (auto port : lib->ports) {
					RTLIL::Wire *port_wire = lib->wire(port);
					if (!port_wire->port_output || port_wire->port_input || cell->hasPort(port))
						continue;
					// One wire per bit: the netlist reader only takes 1-bit wires
					RTLIL::SigSpec sig;
					for (int i = 0; i < port_wire->width; i++)
						sig.append(module->addWire(NEW_ID));
					cell->setPort(port, sig);
					count++;
				}
			}

		log("Connected %d unused output ports.\n", count);
	}
} GowinFixportsPass;

PRIVATE_NAMESPACE_END
