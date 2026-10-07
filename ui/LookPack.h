#pragma once

// A pack of creative looks that ships with the application, as ordinary .cube files.
//
// They are made by this code, not copied from anywhere: plain colour arithmetic (lifts, curves, tints, saturation) written
// out as 33-point look-up tables, so they are small, exact, free to ship, and can be read like any LUT the person adds.
// They are looks, not film-stock emulations: no name here claims to match a product, and none is meant to be technically
// accurate; they work on the display-referred picture the compositor grades, with 0 as black and 1 as white.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace cutline::ui {

struct Look final {
  std::string name;         // also the file name
  std::string description;  // one line, written into the file
  std::array<float, 3> (*map)(float red, float green, float blue);
};

[[nodiscard]] const std::vector<Look>& BuiltInLooks();

// Writes each look as "<name>.cube" into `folder`, creating it. A file the pack wrote before at this version is left alone
// (so the folder is cheap to refresh at every start, and a person's own file of the same name is not overwritten); any
// other file of that name is not touched either. Returns how many files were written.
int WriteBuiltInLooks(const std::filesystem::path& folder, int size = 33);

}  // namespace cutline::ui
