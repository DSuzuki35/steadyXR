#include "workshop_input.hpp"
#include "workshop_playback.hpp"
#include <iostream>
#include <stdexcept>
using namespace vrst::preview;
int main() {
    ButtonEdge edge;
    auto check = [](bool v) {
        if (!v)
            throw std::runtime_error("record button must trigger only a fresh press after release");
    };
    check(!edge.update(true)); // Held at connect must never start recording.
    check(!edge.update(true));
    check(!edge.update(false));
    check(edge.update(true));
    PlaybackClock clock;
    clock.start(1000, 5000000);
    check(!clock.due(1030, 5333333));
    check(clock.due(1040, 5333333));
    check(clock.due(1070, 5666666)); // Delayed first tick must not shift subsequent frames.
    check(!clock.due(1090, 6000000));
    check(clock.due(1100, 6000000));
    check(!edge.update(true));
    edge.disconnect();
    check(!edge.update(true));
    check(!edge.update(false));
    check(edge.update(true));
    std::cout << "Workshop input edge tests passed\n";
}
