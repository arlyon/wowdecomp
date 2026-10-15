#include "Engine/Source/Storm/H/SCritSect.h"

CCritSect::CCritSect() = default;
CCritSect::~CCritSect() = default;

void CCritSect::Enter() {
    this->m_critsect.lock();
}

void CCritSect::Enter(int32_t) {
    this->Enter();
}

void CCritSect::Leave() {
    this->m_critsect.unlock();
}

void CCritSect::Leave(int32_t) {
    this->Leave();
}
