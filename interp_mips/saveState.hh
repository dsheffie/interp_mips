#ifndef __SAVE_STATE_HH__
#define __SAVE_STATE_HH__

#include <string>
#include "profileMips.hh"

void dumpState(const state_t &s, const std::string &filename);
void loadState(state_t &s, const std::string &filename);

#endif
