/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CryptoBuffer_h
#define mozilla_dom_CryptoBuffer_h

#include "nsTArray.h"
#include "seccomon.h"
#include "mozilla/dom/TypedArray.h"

namespace mozilla {
namespace dom {

class ArrayBufferViewOrArrayBuffer;
class OwningArrayBufferViewOrArrayBuffer;

class CryptoBuffer : public FallibleTArray<uint8_t>
{
public:
  CryptoBuffer()
    : FallibleTArray<uint8_t>()
  {}

  template<class T>
  explicit CryptoBuffer(const T& aData)
    : FallibleTArray<uint8_t>()
  {
    Assign(aData);
  }

  template<class T>
  CryptoBuffer& operator=(const T& aData)
  {
    Assign(aData);
    return *this;
  }

  uint8_t* Assign(const uint8_t* aData, uint32_t aLength);
  uint8_t* Assign(const SECItem* aItem);
  uint8_t* Assign(const ArrayBuffer& aData);
  uint8_t* Assign(const ArrayBufferView& aData);
  uint8_t* Assign(const ArrayBufferViewOrArrayBuffer& aData);
  uint8_t* Assign(const OwningArrayBufferViewOrArrayBuffer& aData);

  SECItem* ToSECItem();

  bool GetBigIntValue(unsigned long& aRetVal);
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CryptoBuffer_h
