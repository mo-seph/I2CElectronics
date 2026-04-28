// ElementFactory.h
//
// Creates Element instances from a type_name string (as appears in
// config.json). Returns nullptr for unknown types.
//
// As new Element types are added, add a branch to createElement() in
// ElementFactory.cpp.

#pragma once

#include "Element.h"

class ElementFactory {
public:
    // Returns a newly allocated Element, or nullptr on unknown type.
    // Caller owns the returned pointer.
    static Element* createElement(const char* typeName);
};
