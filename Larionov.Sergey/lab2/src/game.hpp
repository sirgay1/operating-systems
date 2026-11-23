#pragma once

#include <cstdlib>
#include <random>

enum class Outcome { hidden, caught, dead, revived };

inline bool isAlive(Outcome outcome)
{
    return outcome == Outcome::hidden || outcome == Outcome::revived;
}

inline int maximumRoll(bool alive)
{
    return alive ? 100 : 50;
}

inline Outcome evaluateRoll(bool alive, int wolf, int kid, int players)
{
    const int difference = std::abs(wolf - kid);
    if (alive) {
        return difference <= 70 / players ? Outcome::hidden : Outcome::caught;
    }
    return difference <= 20 / players ? Outcome::revived : Outcome::dead;
}

inline int roll(std::mt19937& random, bool alive)
{
    return std::uniform_int_distribution<int>(1, maximumRoll(alive))(random);
}
