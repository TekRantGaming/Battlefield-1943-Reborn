// Our own stand in for ATL's atlbase.h, for DiligentCore's libraries that include it (bf/CMakeLists.txt). Visual
// Studio's Build Tools here have no ATL, and DiligentCore uses only CComPtr from it: in the Win32 file system's
// folder picker and in its DXC wrapper. This is a COM smart pointer with CComPtr's semantics: it holds one reference
// and releases it.

#pragma once

#include <unknwn.h>

template <class T>
class CComPtr {
 public:
  CComPtr() = default;
  CComPtr(T* q) : p(q) {
    if (p) p->AddRef();
  }
  CComPtr(const CComPtr& o) : CComPtr(o.p) {}
  CComPtr(CComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
  ~CComPtr() { Release(); }

  CComPtr& operator=(T* q) {
    if (q) q->AddRef();
    Release();
    p = q;
    return *this;
  }
  CComPtr& operator=(const CComPtr& o) { return *this = o.p; }
  CComPtr& operator=(CComPtr&& o) noexcept {
    if (this != &o) {
      Release();
      p = o.p;
      o.p = nullptr;
    }
    return *this;
  }

  void Release() {
    if (T* t = p) {
      p = nullptr;
      t->Release();
    }
  }
  T* Detach() {
    T* t = p;
    p = nullptr;
    return t;
  }
  void Attach(T* q) {
    Release();
    p = q;
  }
  template <class Q>
  HRESULT QueryInterface(Q** pp) const {
    return p->QueryInterface(__uuidof(Q), reinterpret_cast<void**>(pp));
  }

  T** operator&() { return &p; }
  T* operator->() const { return p; }
  operator T*() const { return p; }
  bool operator!() const { return p == nullptr; }

  T* p = nullptr;
};
