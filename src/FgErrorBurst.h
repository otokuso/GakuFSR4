#pragma once

#include <cstdint>

// Groups consecutive failures by stage and return code without hiding later error bursts.
struct FgErrorBurst
{
    int stage = 0;
    uint32_t code = 0;
    uint32_t consecutive = 0;

    bool Record(int nextStage, uint32_t nextCode)
    {
        if (stage != nextStage || code != nextCode)
        {
            stage = nextStage;
            code = nextCode;
            consecutive = 0;
        }
        if (consecutive < 4) ++consecutive;
        return consecutive <= 3;
    }

    void Reset()
    {
        stage = 0;
        code = 0;
        consecutive = 0;
    }
};
