#pragma once

// Blackmagic's MIDL-generated DeckLink header contains COM forward
// declarations of the form:
//
//     typedef interface IDeckLink ...;
//
// Ensure the Windows COM/RPC definitions (including `interface`) are active
// before parsing it.  Keep this in one wrapper so application sources never
// depend on include-order accidents.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <objbase.h>
#include <Unknwn.h>
#include <OleAuto.h>

#include <DeckLinkAPI_h.h>
