#include "hcs/simulator.hpp"

#include <iostream>

int main() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto out = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto circuit = builder.build();

    hcs::Simulator simulator(circuit);
    for (int av = 0; av <= 1; ++av) {
        for (int bv = 0; bv <= 1; ++bv) {
            simulator.set_input(a, av != 0);
            simulator.set_input(b, bv != 0);
            const auto result = simulator.step();
            std::cout << av << " XOR " << bv << " = " << simulator.signal(out)
                      << " (settled=" << (result.converged ? "yes" : "no") << ")\n";
        }
    }
    return 0;
}
