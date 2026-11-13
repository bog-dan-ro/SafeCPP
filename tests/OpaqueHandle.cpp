/*
MIT No Attribution

Copyright 2026, BogDan Vatra <bogdan@kde.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "TestTypes.h"

namespace tst {

namespace {
int alive = 0;
}

struct Opaque
{
    int value;
};

Opaque *createOpaque(int value)
{
    ++alive;
    return new Opaque{value};
}

void destroyOpaque(Opaque *opaque) noexcept
{
    --alive;
    delete opaque;
}

int opaqueValue(const Opaque *opaque) noexcept
{
    return opaque->value;
}

int opaqueAlive() noexcept
{
    return alive;
}

} // namespace tst
