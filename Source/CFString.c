/* CFString.c
   
   Copyright (C) 2011 Free Software Foundation, Inc.
   
   Written by: Stefan Bidigaray
   Date: May, 2011
   
   This file is part of GNUstep CoreBase Library.
   
   This library is free software; you can redisibute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is disibuted in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; see the file COPYING.LIB.
   If not, see <http://www.gnu.org/licenses/> or write to the 
   Free Software Foundation, 51 Franklin Street, Fifth Floor, 
   Boston, MA 02110-1301, USA.
*/

#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <unicode/uchar.h>
#include <unicode/unorm.h>
#include <unicode/urep.h>
#include <unicode/ustring.h>
#include <unicode/utrans.h>
#include <unicode/ustdio.h>

#include "CoreFoundation/CFRuntime.h"
#include "CoreFoundation/CFBase.h"
#include "CoreFoundation/CFArray.h"
#include "CoreFoundation/CFData.h"
#include "CoreFoundation/CFDictionary.h"
#include "CoreFoundation/CFString.h"
#include "CoreFoundation/CFStringEncodingExt.h"

#include "CoreFoundation/ForFoundationOnly.h"

#define CFRANGE_CHECK(len, range) \
  ((range.location + range.length) <= len)

/* CFString has two possible internal encodings:
     * UTF-16 (preferable)
     * ASCII
   If the encoding is not one of the two listed above, it will be converted
   to UTF-16 any character is not ASCII.
*/

struct __CFString
{
  CFRuntimeBase  _parent;
  void          *_contents;
  CFIndex        _count;
  CFHashCode     _hash;
  CFAllocatorRef _deallocator;
};

struct __CFMutableString
{
  CFRuntimeBase  _parent;
  UniChar       *_contents;
  CFIndex        _count;
  CFHashCode     _hash;
  CFAllocatorRef _allocator;
  CFIndex        _capacity;
};

static CFTypeID _kCFStringTypeID;
static UFILE *_ustdout;

/* These are some masks to access the data in CFRuntimeBase's _flags.info
   field. */
enum
{
  _kCFStringIsMutable = (1<<0),
  _kCFStringIsWide    = (1<<1),
  _kCFStringIsOwned   = (1<<2),
  _kCFStringIsInline  = (1<<3),
  _kCFStringHasLengthByte = (1<<4), // This is used for Pascal strings
  _kCFStringHasNullByte = (1<<5)
};

static inline Boolean
CFStringIsMutable (CFStringRef str)
{
  return
    ((CFRuntimeBase *)str)->_flags.info & _kCFStringIsMutable ? true : false;
}

static inline Boolean
CFStringIsWide (CFStringRef str)
{
  return ((CFRuntimeBase *)str)->_flags.info & _kCFStringIsWide ? true : false;
}

static inline Boolean
CFStringIsOwned (CFStringRef str)
{
  return
    ((CFRuntimeBase *)str)->_flags.info & _kCFStringIsOwned ? true : false;
}

static inline Boolean
CFStringIsInline (CFStringRef str)
{
  return
    ((CFRuntimeBase *)str)->_flags.info & _kCFStringIsInline ? true : false;
}

static inline Boolean
CFStringHasLengthByte (CFStringRef str)
{
  return ((CFRuntimeBase *)str)->_flags.info & _kCFStringHasLengthByte ?
    true : false;
}

static inline Boolean
CFStringHasNullByte (CFStringRef str)
{
  return
    ((CFRuntimeBase *)str)->_flags.info & _kCFStringHasNullByte ? true : false;
}

static inline void
CFStringSetMutable (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringIsMutable;
}

static inline void
CFStringSetWide (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringIsWide;
}

static inline void
CFStringSetOwned (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringIsOwned;
}

static inline void
CFStringSetInline (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringIsInline;
}

static inline void
CFStringSetHasLengthByte (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringHasLengthByte;
}

static inline void
CFStringSetHasNullByte (CFStringRef str)
{
  ((CFRuntimeBase *)str)->_flags.info |= _kCFStringHasNullByte;
}



static void CFStringFinalize (CFTypeRef cf)
{
  CFStringRef str = (CFStringRef)cf;
  
  if (CFStringIsOwned(str) && !CFStringIsInline(str))
    CFAllocatorDeallocate (str->_deallocator, str->_contents);
}

static Boolean CFStringEqual (CFTypeRef cf1, CFTypeRef cf2)
{
  return CFStringCompare (cf1, cf2, 0) == 0 ? true : false;
}

static CFHashCode CFStringHash (CFTypeRef cf)
{
  CFHashCode ret;
  CFIndex len;
  
  CFStringRef str = (CFStringRef)cf;
  if (str->_hash)
    return str->_hash;
  
  /* This must match the NSString hash algorithm. */
  ret = 0;
  len = str->_count;
  if (len > 0)
    {
      register CFIndex idx = 0;
      register const CFIndex len = str->_count;
      UChar32 c;
      
      /* We'll use the _UNSAFE variant of the U*_NEXT macros because
         all bytes stored by CFString are guaranteed to be valid. */
      if (CFStringIsWide(str)) // UTF-16
        {
          register const UniChar *p = str->_contents;
          U16_NEXT_UNSAFE(p, idx, c);
          while (idx < len)
            {
              ret = (ret << 5) + ret + c;
              U16_NEXT_UNSAFE(p, idx, c);
            }
        }
      else // ASCII
        {
          register const char *p = str->_contents;
          c = *p;
          while (idx < len)
            {
              ret = (ret << 5) + ret + c;
              c = p[idx++];
            }
        }
      
      ret &= 0x0fffffff;
      if (ret == 0)
        {
          ret = 0x0fffffff;
        }
    }
  else
    {
      ret = 0x0ffffffe;
    }
  ((struct __CFString *)str)->_hash = ret;

  return str->_hash;
}

static CFStringRef
CFStringCopyFormattingDesc (CFTypeRef cf, CFDictionaryRef formatOptions)
{
  return CFStringCreateCopy(CFGetAllocator(cf), cf);
}

static const CFRuntimeClass CFStringClass =
{
  0,
  "CFString",
  NULL,
  (CFTypeRef (*)(CFAllocatorRef, CFTypeRef))CFStringCreateCopy,
  CFStringFinalize,
  CFStringEqual,
  CFStringHash,
  CFStringCopyFormattingDesc,
  NULL
};

void CFStringInitialize (void)
{
  _kCFStringTypeID = _CFRuntimeRegisterClass (&CFStringClass);
  _ustdout = u_finit (stdout, NULL, NULL);
}



void
CFShow (CFTypeRef obj)
{
  CFStringRef str = CFCopyDescription (obj);
  char *out;
  
  out = CFStringIsWide(str) ? "%S" : "%s";
  
  u_fprintf (_ustdout, out, str->_contents);
}

void
CFShowStr (CFStringRef s)
{
  fprintf (stdout, "Length %d\n", (int)s->_count);
  fprintf (stdout, "IsWide %d\n", CFStringIsWide(s));
  fprintf (stdout, "HasLengthByte %d\n", CFStringHasLengthByte(s));
  fprintf (stdout, "HasNullByte %d\n", CFStringHasNullByte(s));
  fprintf (stdout, "InlineContents %d\n", CFStringIsInline(s));
  fprintf (stdout, "Allocator %p\n", CFGetAllocator(s)); // FIXME: allocator name
  fprintf (stdout, "Mutable %d\n", CFStringIsMutable(s));
  fprintf (stdout, "Contents ");
  u_fprintf (_ustdout, CFStringIsWide(s) ? "%S\n" : "%s\n", s->_contents);
}

CFTypeID
CFStringGetTypeID (void)
{
  return _kCFStringTypeID;
}



/* The CFString create function will return an inlined string whenever
   possible.  The contents may or may not be inlined if a NoCopy function
   is called. With this in mind, CFStringCreateWithBytes will return a
   string with the content inlined and CFStringCreateWithBytesNoCopy will not
   have inlined content if, and only if, the input bytes are in one of the
   internal encodings. */

CFStringRef
CFStringCreateWithBytes (CFAllocatorRef alloc, const UInt8 *bytes,
  CFIndex numBytes, CFStringEncoding encoding,
  Boolean isExternalRepresentation)
{
  struct __CFString *new;
  CFIndex strSize;
  CFIndex size;
  CFVarWidthCharBuffer buffer;
  
  buffer.allocator = alloc;
  if (!__CFStringDecodeByteStream3 (bytes, numBytes, encoding, false, &buffer,
      NULL, 0))
    return NULL;
  
  /* We'll inline the string buffer if __CFStringDecodeByteStream3() has not
     already allocated a buffer for us. */
  if (buffer.shouldFreeChars)
    strSize = 0;
  else
    strSize =
      (buffer.numChars + 1) * (buffer.isASCII ? sizeof(char) : sizeof(UniChar));
  
  size = sizeof(struct __CFString) + strSize - sizeof(struct __CFRuntimeBase);
  new = (struct __CFString *)
    _CFRuntimeCreateInstance (alloc, _kCFStringTypeID, size, NULL);
  
  if (buffer.isASCII)
    {
      memcpy (&(new[1]), buffer.chars.c, buffer.numChars);
      new->_contents = &(new[1]);
      CFStringSetInline((CFStringRef)new);
      
      if (buffer.shouldFreeChars == true)
        CFAllocatorDeallocate (buffer.allocator, buffer.chars.c);
    }
  else
    {
      u_memcpy ((UChar*)&(new[1]), buffer.chars.u, buffer.numChars);
      new->_contents = &(new[1]);
      CFStringSetInline((CFStringRef)new);
      CFStringSetWide((CFStringRef)new);
      
      if (buffer.shouldFreeChars == true)
        CFAllocatorDeallocate (buffer.allocator, buffer.chars.c);
    }
  
  CFStringSetHasNullByte((CFStringRef)new);
  new->_count = buffer.numChars;
  new->_deallocator = alloc;
  
  return (CFStringRef)new;
}

CFStringRef
CFStringCreateWithBytesNoCopy (CFAllocatorRef alloc, const UInt8 *bytes,
  CFIndex numBytes, CFStringEncoding encoding,
  Boolean isExternalRepresentation, CFAllocatorRef contentsDeallocator)
{
  return NULL; // FIXME
}

CFStringRef
CFStringCreateByCombiningStrings (CFAllocatorRef alloc, CFArrayRef theArray,
  CFStringRef separatorString)
{
  CFIndex idx;
  CFIndex count;
  CFMutableStringRef string;
  CFStringRef *strings;
  CFStringRef ret;
  
  count = CFArrayGetCount (theArray);
  if (count == 0)
    return NULL;
  strings = alloca (count * sizeof(CFStringRef));
  CFArrayGetValues (theArray, CFRangeMake(0, count), (const void**)strings);
  
  string = CFStringCreateMutable (NULL, 0);
  idx = 0;
  while (idx < count)
    {
      CFStringRef currentString = strings[idx];
      CFStringAppend (string, currentString);
      CFStringAppend (string, separatorString);
      ++idx;
    }
  
  ret = CFStringCreateCopy (alloc, string);
  CFRelease (string);
  return ret;
}

CFStringRef
CFStringCreateCopy (CFAllocatorRef alloc, CFStringRef str)
{
  CFIndex length;
  CFStringRef new;
  CFStringEncoding enc;
  
  if (alloc == NULL)
    alloc = CFAllocatorGetDefault ();
  
  if (CFGetAllocator(str) == alloc && !CFStringIsMutable(str))
    return CFRetain (str);
  
  length = CFStringIsWide(str) ? str->_count * sizeof(UniChar) : str->_count;
  enc = CFStringIsWide(str) ? kCFStringEncodingUTF16 : kCFStringEncodingASCII;
  new = CFStringCreateWithBytes (alloc, str->_contents, length, enc, false);
  
  return new;
}

CFStringRef
CFStringCreateWithFileSystemRepresentation (CFAllocatorRef alloc,
  const char *buffer)
{
  return NULL; // FIXME
}

CFStringRef
CFStringCreateFromExternalRepresentation (CFAllocatorRef alloc, CFDataRef data,
  CFStringEncoding encoding)
{
  const UInt8 *bytes = CFDataGetBytePtr (data);
  CFIndex numBytes = CFDataGetLength (data);
  return CFStringCreateWithBytes (alloc, bytes, numBytes, encoding, true);
}

CFStringRef
CFStringCreateWithCharacters (CFAllocatorRef alloc, const UniChar *chars,
  CFIndex numChars)
{
#if GS_WORDS_BIGENDIAN
  CFStringEncoding enc = kCFStringENcodingUTF16BE;
#else
  CFStringEncoding enc = kCFStringEncodingUTF16LE;
#endif
  return CFStringCreateWithBytes (alloc, (const UInt8*)chars,
    numChars * sizeof(UniChar), enc, false);
}

CFStringRef
CFStringCreateWithCharactersNoCopy (CFAllocatorRef alloc, const UniChar *chars,
  CFIndex numChars, CFAllocatorRef contentsDeallocator)
{
  return CFStringCreateWithBytesNoCopy (alloc, (const UInt8*)chars,
    numChars * sizeof(UniChar), kCFStringEncodingUnicode, false,
    contentsDeallocator);
}

CFStringRef
CFStringCreateWithCString (CFAllocatorRef alloc, const char *cStr,
  CFStringEncoding encoding)
{
  CFIndex len = strlen(cStr);
  return CFStringCreateWithBytes (alloc, (const UInt8*)cStr, len, encoding,
    false);
}

CFStringRef
CFStringCreateWithCStringNoCopy (CFAllocatorRef alloc, const char *cStr,
  CFStringEncoding encoding, CFAllocatorRef contentsDeallocator)
{
  CFIndex len = strlen(cStr);
  return CFStringCreateWithBytesNoCopy (alloc, (const UInt8*)cStr, len,
    encoding, false, contentsDeallocator);
}

CFStringRef
CFStringCreateWithFormat (CFAllocatorRef alloc, CFDictionaryRef formatOptions,
  CFStringRef format, ...)
{
  CFStringRef result;
  va_list args;
  
  va_start (args, format);
  result =
    CFStringCreateWithFormatAndArguments (alloc, formatOptions, format, args);
  va_end (args);
  
  return result;
}

CFStringRef
CFStringCreateWithFormatAndArguments (CFAllocatorRef alloc,
  CFDictionaryRef formatOptions, CFStringRef format, va_list arguments)
{
  return _CFStringCreateWithFormatAndArgumentsAux (alloc, NULL,
    formatOptions, format, arguments);
}

CFStringRef
CFStringCreateWithSubstring (CFAllocatorRef alloc, CFStringRef str, CFRange range)
{
  return NULL; // FIXME
}

CFDataRef
CFStringCreateExternalRepresentation (CFAllocatorRef alloc,
  CFStringRef str, CFStringEncoding encoding, UInt8 lossByte)
{
  UInt8 *buffer;
  CFRange range;
  CFIndex numBytes;
  CFIndex strLen;
  CFIndex len;
  CFIndex usedLen = 0;
  
  strLen = CFStringGetLength (str);
  range = CFRangeMake (0, strLen);
  len = strLen + 1; // include space for a NULL byte.
  
  buffer = CFAllocatorAllocate (alloc, len, 0);
  
  numBytes = CFStringGetBytes (str, range, encoding, lossByte,
    true, buffer, len, &usedLen);
  
  if (numBytes == 0)
    return NULL;
  
  return CFDataCreateWithBytesNoCopy (alloc, buffer, usedLen, alloc);
}

CFArrayRef
CFStringCreateArrayBySeparatingStrings (CFAllocatorRef alloc,
  CFStringRef str, CFStringRef separatorString)
{
  return NULL; // FIXME
}

const UniChar *
CFStringGetCharactersPtr (CFStringRef str)
{
  return CFStringIsWide(str) ? str->_contents : NULL;
}

const char *
CFStringGetCStringPtr (CFStringRef str, CFStringEncoding enc)
{
  return (!CFStringIsWide(str) && __CFStringEncodingIsSupersetOfASCII(enc))
    ? str->_contents : NULL;
}

CFIndex
CFStringGetBytes (CFStringRef str, CFRange range,
  CFStringEncoding encoding, UInt8 lossByte, Boolean isExternalRepresentation,
  UInt8 *buffer, CFIndex maxBufLen, CFIndex *usedBufLen)
{
  return __CFStringEncodeByteStream (str, range.location, range.length,
    isExternalRepresentation, encoding, (char)lossByte, buffer, maxBufLen,
    usedBufLen);
}

void
CFStringGetCharacters (CFStringRef str, CFRange range, UniChar *buffer)
{
#if GS_WORDS_BIGENDIAN
  CFStringEncoding enc = kCFStringENcodingUTF16BE;
#else
  CFStringEncoding enc = kCFStringEncodingUTF16LE;
#endif
  __CFStringEncodeByteStream (str, range.location, range.length,
    false, enc, '?', (UInt8*)buffer, range.length * sizeof(UniChar), NULL);
}

Boolean
CFStringGetCString (CFStringRef str, char *buffer, CFIndex bufferSize,
  CFStringEncoding encoding)
{
  CFIndex len = CFStringGetLength (str);
  CFIndex used;
  
  if (__CFStringEncodeByteStream (str, 0, len, false, encoding, '?',
      (UInt8*)buffer, bufferSize, &used) == len && used <= len)
    {
      buffer[used] = '\0';
      return true;
    }
  
  return false;
}

Boolean
CFStringGetFileSystemRepresentation (CFStringRef string, char *buffer,
  CFIndex maxBufLen)
{
  return false; // FIXME
}


UniChar
CFStringGetCharacterAtIndex (CFStringRef str, CFIndex idx)
{
  return CFStringIsWide(str) ? ((UniChar*)str->_contents)[idx] :
    ((char*)str->_contents)[idx];
}

CFIndex
CFStringGetLength (CFStringRef str)
{
  return str->_count;
}

CFRange
CFStringGetRangeOfComposedCharactersAtIndex (CFStringRef str,
  CFIndex theIndex)
{
  return CFRangeMake (0, 0); // FIXME
}

UTF32Char
CFStringGetLongCharacterForSurrogatePair (UniChar surrogateHigh,
  UniChar surrogateLow)
{
  return (UTF32Char)U16_GET_SUPPLEMENTARY(surrogateHigh, surrogateLow);
}

Boolean
CFStringGetSurrogatePairForLongCharacter (UTF32Char character,
  UniChar *surrogates)
{
  if (character > 0x10000)
    return false;
  
  surrogates[0] = U16_LEAD(character);
  surrogates[1] = U16_TRAIL(character);
  
  return true;
}

Boolean
CFStringIsSurrogateHighCharacter (UniChar character)
{
  return (Boolean)U16_IS_LEAD(character);
}

Boolean
CFStringIsSurrogateLowCharacter (UniChar character)
{
  return (Boolean)U16_IS_TRAIL(character);
}

CFStringRef
_CFStringCreateWithFormatAndArgumentsAux (CFAllocatorRef alloc,
  CFStringRef (*copyDescFunc)(void *, const void *loc),
  CFDictionaryRef formatOptions, CFStringRef formatString, va_list args)
{
  CFStringRef ret;
  CFMutableStringRef string;
  
  string = CFStringCreateMutable (alloc, 0);
  _CFStringAppendFormatAndArgumentsAux (string, copyDescFunc, formatOptions,
    formatString, args);
  
  ret = CFStringCreateCopy (alloc, string);
  CFRelease (string);
  return ret;
}

double
CFStringGetDoubleValue (CFStringRef str)
{
  return 0.0;
}

SInt32
CFStringGetIntValue (CFStringRef str)
{
  return 0;
}



/* CFMutableString functions start here. 
   
   All mutable string instances are Unicode.  This makes it
   easier to use the ICU functions. */

/* This function is used to grow the size of a CFMutableString buffer.
   The string's buffer will grow to (newCapacity * sizeof(UniChar)).
   On return, oldContentBuffer will point to the old data.  If this value
   is not provided, the old content buffer is freed and data will be lost. */
static Boolean
CFStringCheckCapacityAndGrow (CFMutableStringRef str, CFIndex newCapacity,
  void **oldContentBuffer)
{
  void *currentContents;
  void *newContents;
  struct __CFMutableString *mStr = (struct __CFMutableString *)str;
  
  if (mStr->_capacity >= newCapacity)
    {
      if (oldContentBuffer)
        *oldContentBuffer = str->_contents;
      return true;
    }
  
  currentContents = mStr->_contents;
  
  newContents = CFAllocatorAllocate (mStr->_allocator,
    (newCapacity * sizeof(UniChar)), 0);
  if (newContents == NULL)
      return false;
  mStr->_contents = newContents;
  mStr->_capacity = newCapacity;
  
  if (oldContentBuffer)
    *oldContentBuffer = currentContents;
  else
    CFAllocatorDeallocate (mStr->_allocator, currentContents);
  
  return true;
}

#define DEFAULT_STRING_CAPACITY 16

#define CFSTRING_INIT_MUTABLE(str) do \
{ \
  ((CFRuntimeBase *)str)->_flags.info = \
    0xFF & (_kCFStringIsMutable | _kCFStringIsWide | _kCFStringHasNullByte); \
} while(0)

CFMutableStringRef
CFStringCreateMutable (CFAllocatorRef alloc, CFIndex maxLength)
{
  struct __CFMutableString *new;
  
  new = (struct __CFMutableString *)_CFRuntimeCreateInstance (alloc,
    _kCFStringTypeID,
    sizeof(struct __CFMutableString) - sizeof (CFRuntimeBase),
    NULL);
  
  new->_capacity =
    maxLength < DEFAULT_STRING_CAPACITY ? DEFAULT_STRING_CAPACITY : maxLength;
  new->_allocator = alloc ? alloc : CFAllocatorGetDefault();
  new->_contents = CFAllocatorAllocate (new->_allocator,
    new->_capacity * sizeof(UniChar), 0);
  
  CFSTRING_INIT_MUTABLE(new);
  
  return (CFMutableStringRef)new;
}

CFMutableStringRef
CFStringCreateMutableCopy (CFAllocatorRef alloc, CFIndex maxLength,
  CFStringRef str)
{
  CFMutableStringRef new;
  CFStringInlineBuffer buffer;
  UniChar *contents;
  CFIndex textLen;
  CFIndex idx;
  
  textLen = CFStringGetLength(str);
  if (maxLength < textLen)
    textLen = maxLength;
  new = (CFMutableStringRef)CFStringCreateMutable (alloc, textLen);
  
  // An inline buffer is going to work well here...
  CFStringInitInlineBuffer (str, &buffer, CFRangeMake(0, textLen));
  contents = (UniChar*)new->_contents;
  idx = 0;
  while (idx < textLen)
    {
      UniChar c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
      *(contents++) = c;
    }
  new->_count = textLen;
  
  CFSTRING_INIT_MUTABLE(new);
  
  return new;
}

CFMutableStringRef
CFStringCreateMutableWithExternalCharactersNoCopy (CFAllocatorRef alloc,
  UniChar *chars, CFIndex numChars, CFIndex capacity,
  CFAllocatorRef externalCharactersAllocator)
{
  return NULL;
}

void
CFStringSetExternalCharactersNoCopy (CFMutableStringRef str, UniChar *chars,
  CFIndex length, CFIndex capacity)
{
  return;
}

CFIndex
CFStringFindAndReplace (CFMutableStringRef str, CFStringRef stringToFind,
  CFStringRef replacementString, CFRange rangeToSearch,
  CFOptionFlags compareOptions)
{
  return 0;
}

void
CFStringAppend (CFMutableStringRef str, CFStringRef appendString)
{
  CFStringReplace (str, CFRangeMake(CFStringGetLength(str), 0), appendString);
}

void
CFStringAppendCharacters (CFMutableStringRef str,
  const UniChar *chars, CFIndex numChars)
{
  return; // FIXME
}

void
CFStringAppendCString (CFMutableStringRef str, const char *cStr,
  CFStringEncoding encoding)
{
  UniChar *uStr;
  CFIndex numChars;
  CFVarWidthCharBuffer buffer;
  
  if (encoding == kCFStringEncodingUTF16)
    {
      numChars = u_strlen ((const UChar*)cStr);
      uStr = (UniChar*)cStr;
    }
  else
    {
      buffer.allocator = CFGetAllocator (str);
      /* FIXME: not sure strlen will work here.  If encoding is UTF-16BE or
         UTF-32, for example, you'd expect there to be 0 bytes in multiple
         places. */
      if (!__CFStringDecodeByteStream3 ((const UInt8*)cStr, strlen(cStr),
          encoding, true, &buffer, NULL, 0))
        return; // OH NO!!!
      uStr = buffer.chars.u;
      numChars = buffer.numChars;
    }
  
  CFStringAppendCharacters (str, (const UniChar*)uStr, numChars);
  if (buffer.shouldFreeChars)
    CFAllocatorDeallocate (buffer.allocator, buffer.chars.u);
}

void
CFStringAppendFormat (CFMutableStringRef str, CFDictionaryRef options,
  CFStringRef format, ...)
{
  va_list args;
  va_start (args, format);
  _CFStringAppendFormatAndArgumentsAux (str, NULL, options, format, args);
  va_end (args);
}

void
CFStringAppendFormatAndArguments (CFMutableStringRef str,
  CFDictionaryRef options, CFStringRef format, va_list args)
{
  _CFStringAppendFormatAndArgumentsAux (str, NULL, options, format, args);
}

void
CFStringDelete (CFMutableStringRef str, CFRange range)
{
  CFStringReplace (str, range, CFSTR(""));
}

void
CFStringInsert (CFMutableStringRef str, CFIndex idx, CFStringRef insertedStr)
{
  CFStringReplace (str, CFRangeMake(idx, 0), insertedStr);
}

void
CFStringPad (CFMutableStringRef str, CFStringRef padString,
  CFIndex length, CFIndex indexIntoPad)
{
  return;
}

void
CFStringReplace (CFMutableStringRef str, CFRange range,
  CFStringRef replacement)
{
  CFStringInlineBuffer buffer;
  CFIndex textLength;
  CFIndex repLength;
  CFIndex idx;
  UniChar *contents;
  
  textLength = CFStringGetLength (str);
  repLength = CFStringGetLength (replacement);
  if (!CFRANGE_CHECK(textLength, range))
    return; // out of range
  
  if (repLength != range.length)
    {
      UniChar *moveFrom;
      UniChar *moveTo;
      UniChar *oldContents;
      CFIndex newLength;
      CFIndex moveLength;
      
      newLength = textLength - range.length + repLength;
      if (!CFStringCheckCapacityAndGrow(str, newLength, (void**)&oldContents))
        return;
      moveFrom = (oldContents + range.location + range.length);
      moveTo = (((UniChar*)str->_contents) + range.location + repLength);
      moveLength = textLength - (range.location + range.length);
      memmove (moveTo, moveFrom, moveLength * sizeof(UniChar));
      
      if (oldContents != str->_contents)
        CFAllocatorDeallocate (str->_deallocator, (void*)oldContents);
      
      textLength = newLength;
    }
  
  CFStringInitInlineBuffer (replacement, &buffer, CFRangeMake(0, repLength));
  contents = (((UniChar*)str->_contents) + range.location);
  idx = 0;
  while (idx < repLength)
    {
      UniChar c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
      *(contents++) = c;
    }
  str->_count = textLength;
  str->_hash = 0;
}

void
CFStringReplaceAll (CFMutableStringRef theString, CFStringRef replacement)
{
  CFStringInlineBuffer buffer;
  CFIndex textLength;
  CFIndex idx;
  UniChar *contents;
  
  /* This function is very similar to CFStringReplace() but takes a few
     shortcuts and should be a little fast if all you need to do is replace
     the whole string. */
  textLength = CFStringGetLength (replacement);
  if (!CFStringCheckCapacityAndGrow(theString, textLength + 1, NULL))
    return;
  
  CFStringInitInlineBuffer (replacement, &buffer, CFRangeMake(0, textLength));
  contents = (UniChar*)theString->_contents;
  idx = 0;
  while (idx < textLength)
    {
      UniChar c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
      *(contents++) = c;
    }
  theString->_count = textLength;
  theString->_hash = 0;
}

void
CFStringTrim (CFMutableStringRef str, CFStringRef trimString)
{
  return; // FIXME
}

void
CFStringTrimWhitespace (CFMutableStringRef str)
{
  CFStringInlineBuffer buffer;
  CFIndex start;
  CFIndex end;
  CFIndex textLength;
  CFIndex newLength;
  CFIndex idx;
  UniChar c;
  UniChar *contents;
  UniChar *contentsStart;
  
  /* I assume that the resulting string will be shorter than the original
     so no bounds checking is done. */
  textLength = CFStringGetLength (str);
  CFStringInitInlineBuffer (str, &buffer, CFRangeMake(0, textLength));
  
  idx = 0;
  c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
  while (u_isUWhiteSpace((UChar32)c) && idx < textLength)
    c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
  start = idx - 1;
  end = start;
  while (idx < textLength)
    {
      c = CFStringGetCharacterFromInlineBuffer (&buffer, idx++);
      // reset the end point
      if (!u_isUWhiteSpace((UChar32)c))
        end = idx;
    }
  
  newLength = end - start;
  contents = (UniChar*)str->_contents;
  contentsStart = (UniChar*)(contents + start);
  memmove (contents, contentsStart, newLength * sizeof(UniChar));
  
  str->_count = newLength;
  str->_hash = 0;
}

enum
{
  _kCFStringCapitalize,
  _kCFStringLowercase,
  _kCFStringUppercase,
  _kCFStringFold
};

static void
CFStringCaseMap (CFMutableStringRef str, CFLocaleRef locale,
  CFOptionFlags flags, CFIndex op)
{
  char *localeID = NULL; // FIXME
  const UniChar *oldContents;
  CFIndex oldContentsLength;
  CFIndex newLength;
  int32_t optFlags;
  UErrorCode err = U_ZERO_ERROR;
  struct __CFMutableString *mStr = (struct __CFMutableString *)str;
  
  oldContents = CFStringGetCharactersPtr (str);
  oldContentsLength = CFStringGetLength (str);
  
  /* Loops a maximum of 2 times, and should never loop more than that.  If
     it does have to go through the loop a 3rd time something is wrong
     and this whole thing will blow up. */
  do
    {
      switch (op)
        {
          case _kCFStringCapitalize:
            newLength = u_strToTitle (mStr->_contents, mStr->_capacity,
              oldContents, oldContentsLength, NULL, localeID, &err);
            break;
          case _kCFStringLowercase:
            newLength = u_strToLower (mStr->_contents, mStr->_capacity,
              oldContents, oldContentsLength, localeID, &err);
            break;
          case _kCFStringUppercase:
            newLength = u_strToUpper (mStr->_contents, mStr->_capacity,
              oldContents, oldContentsLength, localeID, &err);
            break;
          case _kCFStringFold:
            optFlags = 0; // FIXME
            newLength = u_strFoldCase (mStr->_contents, mStr->_capacity,
              oldContents, oldContentsLength, optFlags, &err);
            break;
          default:
            return;
        }
    } while (err == U_BUFFER_OVERFLOW_ERROR
        && CFStringCheckCapacityAndGrow(str, newLength, (void**)&oldContents));
  if (U_FAILURE(err))
    return;
  
  mStr->_count = newLength;
  mStr->_hash = 0;
  
  if (oldContents != mStr->_contents)
    CFAllocatorDeallocate (mStr->_allocator, (void*)oldContents);
}

void
CFStringCapitalize (CFMutableStringRef str, CFLocaleRef locale)
{
  CFStringCaseMap (str, locale, 0, _kCFStringCapitalize);
}

void
CFStringLowercase (CFMutableStringRef str, CFLocaleRef locale)
{
  CFStringCaseMap (str, locale, 0, _kCFStringLowercase);
}

void
CFStringUppercase (CFMutableStringRef str, CFLocaleRef locale)
{
  CFStringCaseMap (str, locale, 0, _kCFStringUppercase);
}

void
CFStringFold (CFMutableStringRef str, CFOptionFlags flags, CFLocaleRef locale)
{
  CFStringCaseMap (str, locale, flags, _kCFStringFold);
}

static inline UNormalizationMode
CFToICUNormalization (CFStringNormalizationForm form)
{
  switch (form)
    {
      case kCFStringNormalizationFormD:
        return UNORM_NFD;
      case kCFStringNormalizationFormKD:
        return UNORM_NFKD;
      case kCFStringNormalizationFormC:
        return UNORM_NFC;
      case kCFStringNormalizationFormKC:
        return UNORM_NFKC;
      default:
        return 1;
    }
}

void
CFStringNormalize (CFMutableStringRef str,
  CFStringNormalizationForm theForm)
{
  /* FIXME: The unorm API has been officially deprecated on ICU 4.8, however,
     the new unorm2 API was only introduced on ICU 4.4.  The current plan is
     to provide compatibility down to ICU 4.0, so unorm is used here.  In
     the future, when we no longer support building with older versions of
     the library this code can be updated for the new API. */
  UniChar *oldContents;
  CFIndex oldContentsLength;
  CFIndex newLength;
  UNormalizationCheckResult checkResult;
  UErrorCode err = U_ZERO_ERROR;
  UNormalizationMode mode = CFToICUNormalization (theForm);
  struct __CFMutableString *mStr;
  
  /* Make sure string isn't already normalized.  Use the quick check for
     speed. We still go through the normalization if the quick check does not
     return UNORM_YES. */
  oldContents = (UniChar*)CFStringGetCharactersPtr (str);
  oldContentsLength = CFStringGetLength (str);
  checkResult = unorm_quickCheck (oldContents, oldContentsLength, mode, &err);
  if (U_FAILURE(err) || checkResult == UNORM_YES)
    return;
  
  /* Works just like CFStringCaseMap() above... */
  mStr = (struct __CFMutableString *)str;
  do
    {
      newLength = unorm_normalize (mStr->_contents, mStr->_capacity, mode,
        0, oldContents, oldContentsLength, &err);
    } while (err == U_BUFFER_OVERFLOW_ERROR
        && CFStringCheckCapacityAndGrow(str, newLength, (void**)&oldContents));
  if (U_FAILURE(err))
    return;
  
  mStr->_count = newLength;
  
  if (oldContents != mStr->_contents)
    CFAllocatorDeallocate (mStr->_allocator, (void*)oldContents);
}

Boolean
CFStringTransform (CFMutableStringRef str, CFRange *range,
  CFStringRef transform, Boolean reverse)
{
#define UTRANS_LENGTH 128
  struct __CFMutableString *mStr;
  UniChar transID[UTRANS_LENGTH];
  CFIndex idLength;
  CFIndex newLength;
  CFIndex start; 
  CFIndex limit;
  UTransliterator *utrans;
  UTransDirection dir;
  UErrorCode err = U_ZERO_ERROR;
  
  dir = reverse ? UTRANS_REVERSE : UTRANS_FORWARD;
  
  idLength = CFStringGetLength (transform);
  if (idLength > UTRANS_LENGTH)
    idLength = UTRANS_LENGTH;
  CFStringGetCharacters (transform, CFRangeMake(0, idLength), transID);
  utrans = utrans_openU (transID, idLength, dir, NULL, 0, NULL, &err);
  if (U_FAILURE(err))
    return false;
  
  newLength = CFStringGetLength (str);
  if (range)
    {
      start = range->location;
      limit = range->length + start;
    }
  else
    {
      start = 0;
      limit = newLength;
    }
  
  mStr = (struct __CFMutableString *)str;
  utrans_transUChars (utrans, mStr->_contents, (int32_t*)&mStr->_count,
    mStr->_capacity, start, (int32_t*)&limit, &err);
  if (U_FAILURE(err))
    return false;
  utrans_close (utrans);
  
  if (range)
    range->length = limit;
  
  return true;
}

void
_CFStringAppendFormatAndArgumentsAux (CFMutableStringRef outputString,
  CFStringRef (*copyDescFunc)(void *, const void *loc),
  CFDictionaryRef formatOptions, CFStringRef formatString, va_list args)
{
  // FIXME
}



/* All the Pascal string functions will go here.  None of them are currently
   implemented. */
CFStringRef
CFStringCreateWithPascalString (CFAllocatorRef alloc, ConstStr255Param pStr,
  CFStringEncoding encoding)
{
  return NULL;
}

CFStringRef
CFStringCreateWithPascalStringNoCopy (CFAllocatorRef alloc,
  ConstStr255Param pStr, CFStringEncoding encoding,
  CFAllocatorRef contentsDeallocate)
{
  return NULL;
}

Boolean
CFStringGetPascalString (CFStringRef str, StringPtr buffer,
  CFIndex bufferSize, CFStringEncoding encoding)
{
  return false;
}

ConstStringPtr
CFStringGetPascalStringPtr (CFStringRef str, CFStringEncoding encoding)
{
  if (CFStringHasLengthByte(str))
    return str->_contents;
  
  return NULL;
}

void
CFStringAppendPascalString (CFMutableStringRef str,
  ConstStr255Param pStr, CFStringEncoding encoding)
{
  return;
}



static pthread_mutex_t static_strings_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef static_strings;
/**
 * Hack to allocated CFStrings uniquely without compiler support.
 */
CFStringRef __CFStringMakeConstantString (const char *str)
{
  /* FIXME: Use CFMutableSet as David originally did whenever that type
     gets implemented. */
  CFStringRef new =
    CFStringCreateWithCString (NULL, str, kCFStringEncodingASCII);
  CFStringRef old =
    (CFStringRef)CFDictionaryGetValue (static_strings, (const void *)new);
  // Return the existing string pointer if we have one.
  if (NULL != old)
    {
      CFRelease (new);
      return old;
    }
  pthread_mutex_lock(&static_strings_lock);
  if (NULL == static_strings)
    {
      static_strings = CFDictionaryCreateMutable (NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
  // Check again in case another thread added this string to the table while
  // we were waiting on the mutex.
  old = (CFStringRef)CFDictionaryGetValue (static_strings, (const void *)new);
  if (NULL == old)
    {
      // Note: In theory, for proper retain count tracking, we should release
      // new here.  We're not going to, because it is expected to persist for
      // the lifetime of the program
      CFDictionaryAddValue (static_strings, (const void *)new,
        (const void *)new);
      old = new;
    }
  else
    {
      CFRelease (new);
    }
  pthread_mutex_unlock(&static_strings_lock);
  return old;
}