// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <Standard_Failure.hxx>

#include <exception>
#include <new>

namespace occtwasm {

template <class F>
val guarded(F&& body)
{
  try
  {
    return body();
  }
  catch (const FacadeError& e)
  {
    return errorResult(e.code, e.message);
  }
  catch (const Standard_Failure& e)
  {
    const char* what = e.what();
    std::string message = e.ExceptionType();
    if (what != nullptr && what[0] != '\0')
    {
      message += ": ";
      message += what;
    }
    return errorResult("OCCT_EXCEPTION", message);
  }
  catch (const std::bad_alloc&)
  {
    return errorResult("OUT_OF_MEMORY", "Memory allocation failed");
  }
  catch (const std::exception& e)
  {
    return errorResult("STD_EXCEPTION", e.what());
  }
  catch (...)
  {
    return errorResult("UNKNOWN_EXCEPTION", "Unknown exception");
  }
}

} // namespace occtwasm
