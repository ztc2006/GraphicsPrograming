#include "pch.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

#include "application.hpp"

int main() {
  try {
    Application app;
    app.run();
  } catch (std::exception const &exception) {
    std::cerr << exception.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
