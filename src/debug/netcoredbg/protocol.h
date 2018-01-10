#pragma once

#include <string>

struct Thread
{
    int id;
    std::string name;
    bool running;

    Thread(int id, std::string name, bool running) : id(id), name(name), running(running) {}
};
