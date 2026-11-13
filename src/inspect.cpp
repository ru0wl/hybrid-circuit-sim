#include "hcs/compiler_pipeline.hpp"
#include "hcs/analysis.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string format = argc > 1 ? argv[1] : "text";
    hcs::CircuitBuilder b;
    const auto a = b.add_input();
    const auto c = b.add_input();
    const auto x = b.add_binary(hcs::LogicOp::And, a, c);
    const auto y = b.add_unary(hcs::LogicOp::Not, x);
    const auto circuit = b.build();
    const auto prepared = hcs::prepare_circuit(circuit);
    if (format == "dot") {
        std::cout << "digraph hcs {\n";
        for (std::size_t i=0;i<prepared.dependencies.successors.size();++i)
            for (auto j: prepared.dependencies.successors[i]) std::cout << "  op"<<i<<" -> op"<<j<<";\n";
        std::cout << "}\n";
        return 0;
    }
    if (format == "json") {
        std::cout << "{\"signals\":" << prepared.optimized_ir.signal_count
                  << ",\"operations\":" << prepared.optimized_ir.operations.size()
                  << ",\"regions\":" << prepared.dependencies.regions.size() << "}\n";
        return 0;
    }
    std::cout << "Original IR operations=" << prepared.original_ir.operations.size() << "\n";
    for (std::size_t i=0;i<prepared.original_ir.operations.size();++i) {
        const auto& op=prepared.original_ir.operations[i];
        std::cout << "op "<<i<<" src="<<op.source_component<<" a="<<op.a<<" b="<<op.b<<" out="<<op.output<<"\n";
    }
    std::cout << hcs::analysis::dump(prepared.dependencies);
}
