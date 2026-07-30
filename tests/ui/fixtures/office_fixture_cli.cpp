#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    if (const char *marker = std::getenv("TTC_OFFICE_FIXTURE_MARKER"))
        std::ofstream(marker, std::ios::app) << "started\n";

    if (const char *delay = std::getenv("TTC_OFFICE_FIXTURE_DELAY_MS")) {
        const int delayMs = std::atoi(delay);
        if (delayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    if (argc < 2)
        return 2;

    const std::string command = argv[1];
    if (command == "html") {
        std::cout << "<h1>Fixture document</h1><p>Accepted Office content</p>";
        return 0;
    }
    if (command == "sheets") {
        std::cout << R"([{"name":"Summary","tsv":"Name\tValue\nAlpha\t42\n"},)"
                     R"({"name":"Details","tsv":"Item\tState\nBeta\tReady\n"}])";
        return 0;
    }
    if (command == "svg") {
        std::cout
            << R"(["<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"450\" viewBox=\"0 0 800 450\"><rect width=\"800\" height=\"450\" fill=\"#ffffff\"/><text x=\"60\" y=\"100\" font-size=\"36\">Fixture slide</text></svg>"])";
        return 0;
    }
    if (command == "text") {
        std::cout << "Name\tValue\nAlpha\t42\n";
        return 0;
    }
    return 3;
}
