/* CFURL.c
   
   Copyright (C) 2012 Free Software Foundation, Inc.
   
   Written by: Stefan Bidigaray
   Date: January, 2012
   
   This file is part of the GNUstep CoreBase Library.
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; see the file COPYING.LIB.
   If not, see <http://www.gnu.org/licenses/> or write to the 
   Free Software Foundation, 51 Franklin Street, Fifth Floor, 
   Boston, MA 02110-1301, USA.
*/

/* READ FIRST
 * Using RFC 3986 instead of 2396 because it is the latest URL/URI
 * specification when this file was written in January 2012.  The main
 * difference is that RFC 3986 adds IPv6 address support, allowing
 * this implementation to be a little more future proof than if we were
 * using only RFC 2396.
 * 
 * FIXME
 * The last paragraph on section 3.2.2. Host of RFC 3986 mentions that
 * URI producers should transform non-ASCII DNS registered names to the
 * IDNA encoding.  ICU provides an IDNA API that can be used if something
 * like this needs to be done.
 */

#include "CoreFoundation/CFRuntime.h"
#include "CoreFoundation/CFString.h"
#include "CoreFoundation/CFURL.h"
#include "GSPrivate.h"

#include <string.h>

#if defined(_WIN32)
#define CFURL_DEFAULT_PATH_STYLE kCFURLWindowsPathStyle
#else
#define CFURL_DEFAULT_PATH_STYLE kCFURLPOSIXPathStyle
#endif

#define URL_IS_LEGAL(c) (c > CHAR_SPACE && c < 0x007F)
#define URL_IS_SCHEME(c) (CHAR_IS_ALPHA(c) || CHAR_IS_DIGIT(c) \
  || c == CHAR_PLUS || c == CHAR_MINUS || c == CHAR_PERIOD)
#define URL_IS_PCHAR(c) (URL_IS_UNRESERVED(c) || c == CHAR_PERCENT \
  || URL_IS_SUB_DELIMS(c) || c == CHAR_COLON || c == CHAR_AT)

#define URL_IS_RESERVED(c) (URL_IS_GEN_DELIMS(c) || URL_IS_SUB_DELIMS(c))
#define URL_IS_UNRESERVED(c) (CHAR_IS_ALPHA(c) || CHAR_IS_DIGIT(c) \
  || c == CHAR_MINUS || c == CHAR_PERIOD || c == CHAR_LOW_LINE \
  || c == CHAR_TILDE)

#define URL_IS_GEN_DELIMS(c) (c == CHAR_COLON || c == CHAR_SLASH \
  || c == CHAR_QUESTION || c == CHAR_NUMBER || c == CHAR_L_SQUARE_BRACKET \
  || c == CHAR_R_SQUARE_BRACKET || c == CHAR_AT)
#define URL_IS_SUB_DELIMS(c) (c == CHAR_EXCLAMATION || c == CHAR_DOLLAR \
  || c == CHAR_AMPERSAND || c == CHAR_APOSTROPHE || c == CHAR_L_PARANTHESIS \
  || c == CHAR_ASTERISK || c == CHAR_PLUS || c == CHAR_COMMA \
  || c == CHAR_SEMICOLON || c == CHAR_EQUAL)

static CFTypeID _kCFURLTypeID = 0;

struct __CFURL
{
  CFRuntimeBase _parent;
  CFStringRef   _urlString;
  CFURLRef      _baseURL;
  CFStringEncoding _encoding; // The encoding of the escape characters
  CFRange       _ranges[12]; // CFURLComponentType ranges
};

enum
{
  _kCFURLCanBeDecomposed = (1<<0)
};

CF_INLINE Boolean
CFURLSetCanBeDecomposed (CFURLRef url)
{
  return
    ((CFRuntimeBase *)url)->_flags.info & _kCFURLCanBeDecomposed ? true : false;
}

static void
CFURLFinalize (CFTypeRef cf)
{
  CFURLRef url = (CFURLRef)cf;
  
  CFRelease (url->_urlString);
  if (url->_baseURL)
    CFRelease (url->_baseURL);
}

static const CFRuntimeClass CFURLClass =
{
  0,
  "CFURL",
  NULL,
  NULL,
  CFURLFinalize,
  NULL,
  NULL,
  NULL,
  NULL
};

void CFURLInitialize (void)
{
  _kCFURLTypeID = _CFRuntimeRegisterClass (&CFURLClass);
}



CFTypeID
CFURLGetTypeID (void)
{
  return _kCFURLTypeID;
}

static Boolean
CFURLStringParse (CFStringRef urlString, CFRange ranges[12])
{
  /* URI           = scheme ":" hier-part [ "?" query ] [ "#" fragment ]
   * hier-part     = "//" authority path-abempty
   *               / path-absolute
   *               / path-rootless
   *               / path-empty
   * URI-reference = URI / relative-ref
   * relative-ref  = "//" authority path-abempty
   *               / path-absolute
   *               / path-noscheme
   *               / path-empty
   */
  
  CFStringInlineBuffer iBuffer;
  CFIndex idx;
  CFIndex start;
  CFIndex resourceSpecifierStart;
  CFIndex length;
  UniChar c;
  
  for (idx = 0 ; idx < 12 ; ++idx)
    ranges[idx] = CFRangeMake (kCFNotFound, 0);
  
  length = CFStringGetLength (urlString);
  CFStringInitInlineBuffer (urlString, &iBuffer, CFRangeMake(0, length));
  idx = start = 0;
  resourceSpecifierStart = kCFNotFound;
  
  /* Since ':' can appear anywhere in the string, we'll try to find
   * the scheme first.
   */
  c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
  if (CHAR_IS_ALPHA(c))
    {
      do
        {
          c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
        } while (idx < length && URL_IS_SCHEME(c));
      
      if (c == CHAR_COLON)
        {
          ranges[kCFURLComponentScheme - 1] =
            CFRangeMake (start, idx - start - 1);
        }
      else
        {
          ranges[kCFURLComponentScheme - 1] = CFRangeMake (kCFNotFound, 0);
          idx = 0; // Start over...
        }
      
      start = idx;
      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
    }
  
  /* Parse the relative-ref:
   * hier-part    = "//" authority path-abempty
   *              / path-absolute
   *              / path-rootless
   *              / path-empty
   * relative-ref = "//" authority path-abempty
   *              / path-absolute
   *              / path-noscheme
   *              / path-empty
   */
  if (idx > length) /* path-empty */
    return true;
  
  if (c == CHAR_SLASH)
    {
      /* "//" authority path-abempty
       * path-absolute
       */
      resourceSpecifierStart = idx - 1;
      if (idx > length)
        return true;
          
      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
      if (c == CHAR_SLASH)
        {
          /* "//" authority path-abempty */
          CFIndex end;
          CFIndex i;
          
          i = idx;
          start = i;
          
          if (i >= length) /* empty authority */
            return false;
          
          /* Go to the end so we can parse backwards. */
          do
            {
              c = CFStringGetCharacterFromInlineBuffer (&iBuffer, i++);
              if (!URL_IS_LEGAL(c))
                return false;
            } while (c != CHAR_SLASH && i < length);
          
          if (i == length)
            ranges[kCFURLComponentNetLocation - 1] =
              CFRangeMake (start, i - start);
          else
            ranges[kCFURLComponentNetLocation - 1] =
              CFRangeMake (start, i - start - 1);
          end = i;
          i = start;
          
          while (i < end)
            {
              /* FIXME: RFC 3986 is a little vague on what goes in the
               * UserInfo section, and Apple requires both User Name and
               * Password. We simply consider everything after the first ':'
               * to be part of the password. This behavior needs to be tested.
               */
              c = CFStringGetCharacterFromInlineBuffer (&iBuffer, i++);
              if (c == CHAR_AT)
                {
                  CFIndex userInfoEnd;
                  
                  ranges[kCFURLComponentUserInfo - 1] =
                    CFRangeMake (start, i - start);
                  userInfoEnd = i;
                  
                  i = start;
                  while (i < userInfoEnd)
                    {
                      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, i++);
                      if (c == CHAR_COLON)
                        {
                          ranges[kCFURLComponentPassword - 1] =
                            CFRangeMake (i, userInfoEnd - i - 1);
                          break;
                        }
                    }
                  
                  ranges[kCFURLComponentUser - 1] =
                    CFRangeMake (start, i - start - 1);
                  start = userInfoEnd;
                  break;
                }
            }
          
          i = end;
          idx = end; // Set idx before we move it to search for a port.
          
          /* Try to find a port. */
          while (i > start)
            {
              c = CFStringGetCharacterFromInlineBuffer (&iBuffer, --i);
              if (c == CHAR_COLON)
                {
                  ranges[kCFURLComponentPort - 1] = CFRangeMake (i, end - i);
                  end = i;
                  break;
                }
            }
          /* Whatever's left is the host name. */
          ranges[kCFURLComponentHost - 1] =
            CFRangeMake (start, end - start - 1);
          
          c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx);
          start = idx - 1; // Shift back to just before '/'
        }
    }
  
  if (idx < length)
    {
      while (idx < length)
        {
          if (c == CHAR_QUESTION || c == CHAR_NUMBER)
            break;
          if (!URL_IS_LEGAL(c))
            return false;
          c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
        }
      if (idx < length)
        ranges[kCFURLComponentPath - 1] = CFRangeMake (start, idx - start - 1);
      else
        ranges[kCFURLComponentPath - 1] = CFRangeMake (start, idx - start);
    }
  
  if (idx < length && c == CHAR_QUESTION)
    {
      start = idx;
      while (idx < length)
        {
          c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
          if (c == CHAR_NUMBER)
            break;
          if (!(URL_IS_PCHAR(c) || c == CHAR_SLASH || c == CHAR_QUESTION))
            return false;
        }
      if (c == CHAR_NUMBER)
        ranges[kCFURLComponentQuery - 1] = CFRangeMake (start, idx - start - 1);
      else
        ranges[kCFURLComponentQuery - 1] = CFRangeMake (start, idx - start);
    }
  
  if (idx < length && c == CHAR_NUMBER)
    {
      start = idx;
      while (idx < length)
        {
          c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx++);
          if (!(URL_IS_PCHAR(c) || c == CHAR_SLASH || c == CHAR_QUESTION))
            return false;
        }
      ranges[kCFURLComponentFragment - 1] =
        CFRangeMake (start, idx - start);
    }
  
  ranges[kCFURLComponentResourceSpecifier - 1] =
    CFRangeMake (resourceSpecifierStart, idx - resourceSpecifierStart);
  
  return true;
}

#define CFURL_SIZE sizeof(struct __CFURL) - sizeof(CFRuntimeBase)

static CFURLRef
CFURLCreate_internal (CFAllocatorRef alloc, CFStringRef string,
  CFURLRef baseURL, CFStringEncoding encoding)
{
  struct __CFURL *new;
  CFRange ranges[12];
  
  if (!CFURLStringParse (string, ranges))
    return NULL;
  
  new = (struct __CFURL*)_CFRuntimeCreateInstance (alloc, _kCFURLTypeID,
    CFURL_SIZE, 0);
  if (new)
    {
      new->_urlString = CFStringCreateCopy (alloc, string);
      if (ranges[kCFURLComponentScheme - 1].location == kCFNotFound
          && baseURL)
        new->_baseURL = CFURLCopyAbsoluteURL (baseURL);
      else
        new->_baseURL = NULL;
      new->_encoding = encoding;
      memcpy (new->_ranges, ranges, sizeof(ranges));
    }
  
  return new;
}

CFURLRef
CFURLCreateWithString (CFAllocatorRef alloc, CFStringRef string,
  CFURLRef baseURL)
{
  return CFURLCreate_internal (alloc, string, baseURL, kCFStringEncodingUTF8);
}

static void
CFURLStringAppendByRemovingDotSegments (CFMutableStringRef string,
  UniChar *buffer, CFIndex length)
{
  UniChar *bufferStart;
  UniChar *bufferEnd;
  CFIndex pathStart;
  
  bufferStart = buffer;
  bufferEnd = bufferStart + length;
  pathStart = CFStringGetLength (string);
  
  /* Skip any '../' and './' */
  if (buffer < bufferEnd && *buffer == CHAR_PERIOD)
    {
      if ((buffer + 1) < bufferEnd && buffer[1] == CHAR_SLASH)
        {
          buffer += 1;
        }
      else if ((buffer + 2) < bufferEnd && buffer[1] == CHAR_PERIOD
          && buffer[2] == CHAR_SLASH)
        {
          buffer += 2;
        }
    }
  bufferStart = buffer;
  
  /* Start checking for '/.' and '/..' */
  while (buffer < bufferEnd)
    {
      if ((buffer + 1) < bufferEnd && buffer[0] == CHAR_SLASH
          && buffer[1] == CHAR_PERIOD)
        {
          /* Skip '/./' or '/.'EOS */
          if (((buffer + 2) < bufferEnd && buffer[2] == CHAR_SLASH)
              || (buffer + 2) == bufferEnd)
            {
              if ((buffer + 2) == bufferEnd)
                {
                  buffer[1] = CHAR_SLASH;
                  buffer += 1;
                }
              else
                {
                  buffer += 2;
                }
            }
          /* Skip '/../' or '/..'EOS */
          else if (((buffer + 3) < bufferEnd && buffer[2] == CHAR_PERIOD
                && buffer[3] == CHAR_SLASH)
                || ((buffer + 3) == bufferEnd && buffer[2] == CHAR_PERIOD))
            {
              CFStringInlineBuffer iBuffer;
              UniChar c;
              CFIndex i;
              CFIndex pathLength;
              
              pathLength = CFStringGetLength(string) - pathStart;
              CFStringInitInlineBuffer (string, &iBuffer,
                CFRangeMake(pathStart, pathLength));
              i = pathLength - 1;
              while (i >= 0)
                {
                  c = CFStringGetCharacterFromInlineBuffer (&iBuffer, i--);
                  if (c == CHAR_SLASH)
                    break;
                }
              CFStringDelete (string,
                CFRangeMake(pathStart + i + 1, pathLength - i - 1));
              
              if ((buffer + 3) == bufferEnd)
                {
                  buffer[2] = CHAR_SLASH;
                  buffer += 2;
                }
              else
                {
                  buffer += 3;
                }
            }
          /* Something like '/..*' */
          else
            {
              /* Skip onto the next '/' */
              do
                {
                  buffer++;
                } while (buffer < bufferEnd && *buffer != CHAR_SLASH);
              CFStringAppendCharacters (string, bufferStart,
                buffer - bufferStart);
            }
        }
      else
        {
          /* Skip onto the next '/' */
          do
            {
              buffer++;
            } while (buffer < bufferEnd && *buffer != CHAR_SLASH);
          CFStringAppendCharacters (string, bufferStart, buffer - bufferStart);
        }
      bufferStart = buffer;
    }
}

CFURLRef
CFURLCopyAbsoluteURL (CFURLRef relativeURL)
{
  CFAllocatorRef alloc;
  CFMutableStringRef targetString;
  CFStringRef relString;
  CFStringRef baseString;
  CFURLRef base;
  CFURLRef target;
  UniChar *buffer;
  CFIndex capacity;
  CFRange *relRanges;
  CFRange baseRanges[12];
  CFRange range;
  
  base = relativeURL->_baseURL;
  if (base == NULL)
    return CFRetain (relativeURL);
  
  /* This is a bit of a pain.  We can't assume _baseURL is a CFURL, so we
   * need to parse it before moving forward.  To avoid parsing the same
   * string twice (in the case that _baseURL is a CFURL) we check first.
   */
  baseString = CFURLGetString (base);
  if (CF_IS_OBJC(_kCFURLTypeID, base))
    CFURLStringParse (baseString, baseRanges);
  else
    memcpy (baseRanges, base->_ranges, sizeof(baseRanges));
  
  relString = relativeURL->_urlString;
  relRanges = (CFRange*)relativeURL->_ranges;
  
  alloc = CFGetAllocator(relativeURL);
  capacity = CFStringGetLength(relString) + CFStringGetLength(baseString);
  buffer = CFAllocatorAllocate (alloc, capacity * sizeof(UniChar), 0);
  targetString = CFStringCreateMutable (alloc, capacity);
  
  range = relRanges[kCFURLComponentScheme - 1];
  if (range.location != kCFNotFound)
    {
      /* Scheme */
      CFStringGetCharacters (relString, range, buffer);
      CFStringAppendCharacters (targetString, buffer, range.length);
      CFStringAppendCString (targetString, ":", kCFStringEncodingASCII);
      
      /* Authority */
      range = relRanges[kCFURLComponentNetLocation - 1];
      if (range.location != kCFNotFound)
        {
          CFStringAppendCString (targetString, "//", kCFStringEncodingASCII);
          CFStringGetCharacters (relString, range, buffer);
          CFStringAppendCharacters (targetString, buffer, range.length);
        }
      
      /* Path */
      range = relRanges[kCFURLComponentPath - 1];
      if (range.location != kCFNotFound)
        {
          CFStringGetCharacters (relString, range, buffer);
          CFURLStringAppendByRemovingDotSegments (targetString, buffer,
            range.length);
        }
      
      /* Query */
      range = relRanges[kCFURLComponentQuery - 1];
      if (range.location != kCFNotFound)
        {
          CFStringAppendCString (targetString, "?", kCFStringEncodingASCII);
          CFStringGetCharacters (relString, range, buffer);
          CFStringAppendCharacters (targetString, buffer, range.length);
        }
    }
  else
    {
      /* Scheme */
      range = baseRanges[kCFURLComponentScheme - 1];
      if (range.location != kCFNotFound)
        {
          CFStringGetCharacters (baseString, range, buffer);
          CFStringAppendCharacters (targetString, buffer, range.length);
          CFStringAppendCString (targetString, ":", kCFStringEncodingASCII);
        }
      
      range = relRanges[kCFURLComponentNetLocation - 1];
      if (range.location != kCFNotFound)
        {
          /* Authority */
          CFStringAppendCString (targetString, "//", kCFStringEncodingASCII);
          CFStringGetCharacters (relString, range, buffer);
          CFStringAppendCharacters (targetString, buffer, range.length);
          
          /* Path */
          range = relRanges[kCFURLComponentPath - 1];
          if (range.location != kCFNotFound)
            {
              CFStringGetCharacters (relString, range, buffer);
              CFURLStringAppendByRemovingDotSegments (targetString, buffer,
                range.length);
            }
          
          /* Query */
          range = relRanges[kCFURLComponentQuery - 1];
          if (range.location != kCFNotFound)
            {
              CFStringAppendCString (targetString, "?",
                kCFStringEncodingASCII);
              CFStringGetCharacters (relString, range, buffer);
              CFStringAppendCharacters (targetString, buffer, range.length);
            }
        }
      else
        {
          /* Authority */
          range = baseRanges[kCFURLComponentNetLocation - 1];
          if (range.location != kCFNotFound)
            {
              CFStringAppendCString (targetString, "//",
                kCFStringEncodingASCII);
              CFStringGetCharacters (baseString, range, buffer);
              CFStringAppendCharacters (targetString, buffer, range.length);
            }
          
          range = relRanges[kCFURLComponentPath - 1];
          if (range.location != kCFNotFound)
            {
              /* Path */
              if (range.length == 0)
                {
                  range = baseRanges[kCFURLComponentPath - 1];
                  if (range.location != kCFNotFound)
                    {
                      CFStringGetCharacters (baseString, range, buffer);
                      CFStringAppendCharacters (targetString, buffer,
                        range.length);
                    }
                  
                  /* Query */
                  range = relRanges[kCFURLComponentQuery - 1];
                  if (range.location != kCFNotFound)
                    {
                      CFStringAppendCString (targetString, "?",
                        kCFStringEncodingASCII);
                      CFStringGetCharacters (relString, range, buffer);
                      CFStringAppendCharacters (targetString, buffer,
                        range.length);
                    }
                  else
                    {
                      /* Query */
                      range = baseRanges[kCFURLComponentQuery - 1];
                      if (range.location != kCFNotFound)
                        {
                          CFStringAppendCString (targetString, "?",
                            kCFStringEncodingASCII);
                          CFStringGetCharacters (baseString, range, buffer);
                          CFStringAppendCharacters (targetString, buffer,
                            range.length);
                        }
                    }
                }
              else
                {
                  if (CFStringGetCharacterAtIndex(relString, range.location)
                      == CHAR_SLASH)
                    {
                      CFStringGetCharacters (relString, range, buffer);
                      CFURLStringAppendByRemovingDotSegments (targetString,
                        buffer, range.length);
                    }
                  else
                    {
                      CFRange baseRange;
                      
                      baseRange = baseRanges[kCFURLComponentPath - 1];
                      
                      CFStringGetCharacters (baseString, baseRange, buffer);
                      if (baseRange.location != kCFNotFound
                          && buffer[baseRange.length - 1] != CHAR_SLASH)
                        {
                          /* Remove last path component */
                          CFIndex count;
                          count = baseRange.length - 1;
                          while (buffer[--count] != CHAR_SLASH);
                          baseRange.length = count + 1;
                        }
                      CFStringGetCharacters (relString, range,
                        &buffer[baseRange.length]);
                      range.length += baseRange.length;
                      
                      CFURLStringAppendByRemovingDotSegments (targetString,
                        buffer, range.length);
                    }
                  
                  range = relRanges[kCFURLComponentQuery - 1];
                  if (range.location != kCFNotFound)
                    {
                      CFStringAppendCString (targetString, "?",
                        kCFStringEncodingASCII);
                      CFStringGetCharacters (relString, range, buffer);
                      CFStringAppendCharacters (targetString, buffer,
                        range.length);
                    }
                }
            }
        }
    }
  
  /* Fragment */
  range = relRanges[kCFURLComponentFragment - 1];
  if (range.location != kCFNotFound)
    {
      CFStringAppendCString (targetString, "#", kCFStringEncodingASCII);
      CFStringGetCharacters (relString, range, buffer);
      CFStringAppendCharacters (targetString, buffer, range.length);
    }
  
  target = CFURLCreate_internal (alloc, targetString, NULL,
    kCFStringEncodingUTF8);
  CFRelease (targetString);
  CFAllocatorDeallocate (alloc, buffer);
  
  return target;
}

CFURLRef
CFURLCreateAbsoluteURLWithBytes (CFAllocatorRef alloc,
  const UInt8 *relativeURLBytes, CFIndex length, CFStringEncoding encoding,
  CFURLRef baseURL, Boolean useCompatibilityMode)
{
  // FIXME: what to do with useCompatibilityMode?
  CFURLRef url;
  CFStringRef str;
  
  str = CFStringCreateWithBytes (alloc, relativeURLBytes, length,
    encoding, false);
  if (str == NULL)
    return NULL;
  
  url = CFURLCreate_internal (alloc, str, baseURL, encoding);
  if (url)
    {
      CFURLRef tmp = CFURLCopyAbsoluteURL (url);
      CFRelease (url);
      url = tmp;
    }
  
  return url;
}

CFURLRef
CFURLCreateByResolvingBookmarkData (CFAllocatorRef alloc, CFDataRef bookmark,
  CFURLBookmarkResolutionOptions options, CFURLRef relativeToURL,
  CFArrayRef resourcePropertiesToInclude, Boolean *isStale, CFErrorRef *error)
{
  return NULL; // FIXME: ???
}

CFURLRef
CFURLCreateCopyAppendingPathComponent (CFAllocatorRef alloc, CFURLRef url,
  CFStringRef pathComponent, Boolean isDirectory)
{
  CFURLRef ret;
  CFMutableStringRef str;
  
  str = CFStringCreateMutableCopy (alloc, 0, CFURLGetString(url));
  // FIXME: check if last component is dir or file...
  CFStringAppend (str, pathComponent);
  
  ret = CFURLCreate_internal (alloc, str, url->_baseURL, url->_encoding);
  CFRelease (str);
  
  return ret;
}

CFURLRef
CFURLCreateCopyAppendingPathExtension (CFAllocatorRef alloc, CFURLRef url,
  CFStringRef extension)
{
  CFURLRef ret;
  CFMutableStringRef str;
  
  str = CFStringCreateMutableCopy (alloc, 0, CFURLGetString(url));
  // FIXME: check if last component is dir or file...
  CFStringAppend (str, extension);
  
  ret = CFURLCreate_internal (alloc, str, url->_baseURL, url->_encoding);
  CFRelease (str);
  
  return ret;
}

CFURLRef
CFURLCreateCopyDeletingLastPathComponent (CFAllocatorRef alloc, CFURLRef url)
{
  return NULL;
}

CFURLRef
CFURLCreateCopyDeletingPathExtension (CFAllocatorRef alloc, CFURLRef url)
{
  return NULL;
}

CFURLRef
CFURLCreateFilePathURL (CFAllocatorRef alloc, CFURLRef url,
  CFErrorRef *error)
{
  return NULL;
}

CFURLRef
CFURLCreateFileReferenceURL (CFAllocatorRef alloc, CFURLRef url,
  CFErrorRef *error)
{
  return NULL; // FIXME ???
}

CFURLRef
CFURLCreateFromFileSystemRepresentation (CFAllocatorRef alloc,
  const UInt8 *buffer, CFIndex bufLen, Boolean isDirectory)
{
  return CFURLCreateFromFileSystemRepresentationRelativeToBase (alloc,
    buffer, bufLen, isDirectory, NULL);
}

CFURLRef
CFURLCreateFromFileSystemRepresentationRelativeToBase (CFAllocatorRef alloc,
  const UInt8 *buffer, CFIndex bufLen, Boolean isDirectory, CFURLRef baseURL)
{
  CFURLRef ret;
  CFStringRef path;
  
  path = CFStringCreateWithBytesNoCopy (alloc, buffer, bufLen,
    GSStringGetFileSystemEncoding(), false, kCFAllocatorNull);
  ret = CFURLCreateWithFileSystemPathRelativeToBase (alloc, path,
    CFURL_DEFAULT_PATH_STYLE, isDirectory, baseURL);
  
  CFRelease (path);
  
  return ret;
}

CFURLRef
CFURLCreateWithFileSystemPath (CFAllocatorRef alloc,
 CFStringRef fileSystemPath, CFURLPathStyle style, Boolean isDirectory)
{
  CFURLRef ret;
  
  ret = CFURLCreateWithFileSystemPathRelativeToBase (alloc, fileSystemPath,
    style, isDirectory, NULL);
  
  return ret;
}

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <string.h>
#endif

CF_INLINE CFURLRef
CFURLCreateWithCurrentDirectory (CFAllocatorRef alloc)
{
  CFURLRef ret;
  CFStringRef cwd;
  CFMutableStringRef str;
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH];
  DWORD length;
  
  length = GetCurrentDirectoryW (MAX_PATH, buffer);
  if (length == 0)
    return NULL;
  
  cwd = CFStringCreateWithBytesNoCopy (alloc, (const UInt8 *)buffer, length,
    GSStringGetFileSystemEncoding(), false, kCFAllocatorNull);
#else
  char buffer[1024];
  
  if (getcwd(buffer, 1024) == NULL)
    return NULL;
  
  cwd = CFStringCreateWithBytesNoCopy (alloc, (const UInt8 *)buffer,
    strlen(buffer), GSStringGetFileSystemEncoding(), false, kCFAllocatorNull);
#endif
  
  str = CFStringCreateMutable (alloc, 0);
  CFStringAppend (str, CFSTR("file://localhost/"));
  CFStringAppend (str, cwd);
  
  ret = CFURLCreateWithString (alloc, str, NULL);
  CFRelease (cwd);
  CFRelease (str);
  
  return ret;
}

CFURLRef
CFURLCreateWithFileSystemPathRelativeToBase (CFAllocatorRef alloc,
  CFStringRef filePath, CFURLPathStyle style, Boolean isDirectory,
  CFURLRef baseURL)
{
  CFURLRef ret;
  Boolean abs;
  UniChar delim;
  CFIndex filePathLength;
  
  switch (style)
    {
      case kCFURLPOSIXPathStyle:
        abs = (CFStringGetCharacterAtIndex(filePath, 0) == CHAR_SLASH);
        delim = CHAR_SLASH;
        break;
      case kCFURLHFSPathStyle:
        // FIXME: I don't know how to handle this!
        abs = (CFStringGetCharacterAtIndex(filePath, 0) == CHAR_COLON);
        delim = CHAR_COLON;
      case kCFURLWindowsPathStyle:
        abs = (CFStringGetCharacterAtIndex(filePath, 1) == CHAR_COLON
          && CFStringGetCharacterAtIndex(filePath, 2) == CHAR_BACKSLASH);
        delim = CHAR_BACKSLASH;
        break;
      default:
        return NULL;
    }
  
  if (abs)
    baseURL = NULL;
  else if (baseURL == NULL)
    baseURL = CFURLCreateWithCurrentDirectory (alloc);
  else
    CFRetain (baseURL);
  
  filePathLength = CFStringGetLength (filePath);
  CFRetain (filePath);
  if (isDirectory)
    {
      if (CFStringGetCharacterAtIndex(filePath, filePathLength - 1) != delim)
        {
          CFRelease (filePath);
          filePath = (CFStringRef)
            CFStringCreateMutableCopy (alloc, filePathLength + 1, filePath);
          CFStringAppendCharacters ((CFMutableStringRef)filePath, &delim, 1);
        }
    }
  else
    {
      if (CFStringGetCharacterAtIndex(filePath, filePathLength - 1) == delim)
        {
          CFRelease (filePath);
          filePath =
            CFStringCreateMutableCopy (alloc, filePathLength, filePath);
          CFStringDelete ((CFMutableStringRef)filePath,
            CFRangeMake(filePathLength - 1, 1));
        }
    }
  
  /* We don't need to worry about percent escapes since there won't be
   * any for the file system path.  We pass 0 for the encoding.
   */
  ret = CFURLCreate_internal (alloc, filePath, baseURL, 0);
  
  CFRelease (filePath);
  if (baseURL)
    CFRelease (baseURL);
  
  return ret;
}

CFURLRef
CFURLCreateWithBytes (CFAllocatorRef alloc, const UInt8 *bytes, CFIndex length,
  CFStringEncoding encoding, CFURLRef baseURL)
{
  CFStringRef str;
  CFURLRef ret;
  
  str = CFStringCreateWithBytesNoCopy (NULL, bytes, length, encoding, false,
    kCFAllocatorNull);
  ret = CFURLCreate_internal (alloc, str, baseURL, encoding);
  
  CFRelease (str);
  
  return ret;
}

Boolean
CFURLCanBeDecomposed (CFURLRef url)
{
  if (((CFRuntimeBase*)url)->_flags.info & _kCFURLCanBeDecomposed)
    return true;
  else
    return url->_baseURL ? CFURLCanBeDecomposed(url->_baseURL) : false;
}

CFStringRef
CFURLCopyFileSystemPath (CFURLRef url, CFURLPathStyle style)
{
  CFRange range = url->_ranges[kCFURLComponentPath - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyFileSystemPath (url->_baseURL, style);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyFragment (CFURLRef url, CFStringRef charactersToLeaveEscaped)
{
  CFRange range = url->_ranges[kCFURLComponentFragment - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyFragment (url->_baseURL, charactersToLeaveEscaped);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyHostName (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentHost - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyHostName (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyLastPathComponent (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentPath - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyLastPathComponent (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyNetLocation (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentNetLocation - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyNetLocation (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyParameterString (CFURLRef url, CFStringRef charactersToLeaveEscaped)
{
  CFRange range = url->_ranges[kCFURLComponentParameterString - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyParameterString (url->_baseURL, charactersToLeaveEscaped);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyPassword (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentPassword - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyPassword (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyPath (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentPath - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyPath (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyPathExtension (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentPath - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyPathExtension (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyQueryString (CFURLRef url, CFStringRef charactersToLeaveEscaped)
{
    CFRange range = url->_ranges[kCFURLComponentQuery - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyQueryString (url->_baseURL, charactersToLeaveEscaped);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyResourceSpecifier (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentResourceSpecifier - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyResourceSpecifier (url->_baseURL);
      return CFSTR(""); // FIXME
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyScheme (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentScheme - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyScheme (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyStrictPath (CFURLRef url, Boolean *isAbsolute)
{
CFRange range = url->_ranges[kCFURLComponentPath - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyStrictPath (url->_baseURL, isAbsolute);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

CFStringRef
CFURLCopyUserName (CFURLRef url)
{
  CFRange range = url->_ranges[kCFURLComponentUser - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLCopyUserName (url->_baseURL);
      return NULL;
    }
  return CFStringCreateWithSubstring (CFGetAllocator(url), url->_urlString,
    range);
}

SInt32
CFURLGetPortNumber (CFURLRef url)
{
CFRange range = url->_ranges[kCFURLComponentPort - 1];
  if (range.location == kCFNotFound)
    {
      if (url->_baseURL)
        return CFURLGetPortNumber (url->_baseURL);
      return 0;
    }
  return 0;
}

Boolean
CFURLHasDirectoryPath (CFURLRef url)
{
  CFStringRef str;
  CFIndex len;
  
  str = CFURLGetString (url);
  len = CFStringGetLength (str);
  
  return (CHAR_SLASH == CFStringGetCharacterAtIndex (str, len - 1));
}

CFDataRef
CFURLCreateData (CFAllocatorRef alloc, CFURLRef url, CFStringEncoding encoding,
  Boolean escapeWhiteSpace)
{
  CFDataRef ret;
  CFURLRef abs;
  CFStringRef absStr;
  
  abs = CFURLCopyAbsoluteURL (url);
  absStr = CFURLGetString (abs);
  if (escapeWhiteSpace)
    absStr = CFURLCreateStringByAddingPercentEscapes (alloc, absStr, NULL,
      CFSTR(" \r\n\t"), encoding);
  ret = CFStringCreateExternalRepresentation (alloc, absStr, encoding, 0);
  
  if (escapeWhiteSpace)
    CFRelease (absStr);
  CFRelease (abs);
  
  return ret;
}

static Boolean
CFURLAppendPercentEscapedForCharacter (char **dst, UniChar c,
  CFStringEncoding enc)
{
  CFIndex len;
  char buffer[8]; // 8 characters should be more than enough for any encoding.
  const UniChar *source;
  
  source = &c;
  if ((len =
      GSStringEncodingFromUnicode(enc, buffer, 8, &source, 1, 0, false, NULL)))
    {
      char hi;
      char lo;
      char *target;
      const char *end;
      
      target = buffer;
      end = target + len;
      do
        {
          (*(*dst)++) = '%';
          hi = ((*target >> 4) & 0x0F);
          lo = (*target & 0x0F);
          (*(*dst)++) = (hi > 9) ? hi + 'A' - 10 : hi + '0';
          (*(*dst)++) = (lo > 9) ? lo + 'A' - 10 : lo + '0';
          
          ++target;
        } while (target < end);
      
      return true;
    }
  
  return false;
}


static Boolean
CFURLStringContainsCharacter (CFStringRef toEscape, UniChar ch)
{
  CFStringInlineBuffer iBuffer;
  CFIndex sLength;
  CFIndex i;
  UniChar c;
  
  sLength = CFStringGetLength (toEscape);
  CFStringInitInlineBuffer (toEscape, &iBuffer, CFRangeMake (0, sLength));
  for (i = 0 ; i < sLength ; ++i)
    {
      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, i);
      if (c == ch)
        return true;
    }
  
  return false;
}

CF_INLINE Boolean
CFURLShouldEscapeCharacter (UniChar c, CFStringRef leaveUnescaped,
  CFStringRef toEscape)
{
  if (URL_IS_UNRESERVED(c) || URL_IS_RESERVED(c))
    {
      if (toEscape && CFURLStringContainsCharacter(toEscape, c))
        return true;
      
      return false;
    }
  
  if (leaveUnescaped && CFURLStringContainsCharacter(leaveUnescaped, c))
    return false;
  
  return true;
}

CFStringRef
CFURLCreateStringByAddingPercentEscapes (CFAllocatorRef alloc,
  CFStringRef origString, CFStringRef leaveUnescaped,
  CFStringRef toEscape, CFStringEncoding encoding)
{
  CFStringInlineBuffer iBuffer;
  CFStringRef ret;
  CFIndex sLength;
  CFIndex idx;
  char *dst;
  char *dpos;
  UniChar c;
  
  sLength = CFStringGetLength (origString);
  CFStringInitInlineBuffer (origString, &iBuffer, CFRangeMake (0, sLength));
  
  dst = CFAllocatorAllocate (alloc, sizeof(char) * sLength * 3, 0);
  dpos = dst;
  for (idx = 0 ; idx < sLength ; ++idx)
    {
      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx);
      if (CFURLShouldEscapeCharacter(c, leaveUnescaped, toEscape))
        {
          if (!CFURLAppendPercentEscapedForCharacter (&dpos, c, encoding))
            {
              CFAllocatorDeallocate (alloc, dst);
              return NULL;
            }
        }
      else
        {
          (*dpos++) = (char)c;
        }
    }
  
  ret = CFStringCreateWithBytes (alloc, (UInt8*)dst, (CFIndex)(dpos - dst),
    kCFStringEncodingASCII, false);
  CFAllocatorDeallocate (alloc, dst);
  
  return ret;
}

CFStringRef
CFURLCreateStringByReplacingPercentEscapes (CFAllocatorRef alloc,
  CFStringRef origString, CFStringRef leaveEscaped)
{
  return CFURLCreateStringByReplacingPercentEscapesUsingEncoding (alloc,
    origString, leaveEscaped, kCFStringEncodingUTF8);
}

CF_INLINE char
CFURLCharacterForPercentEscape (CFStringInlineBuffer *src, CFIndex *idx,
  CFStringEncoding enc)
{
  UInt8 bytes[8];
  UInt8 tmp;
  UInt8 *str;
  UniChar current;
  UniChar c;
  CFIndex num;
  CFIndex i;
  CFIndex j;
  
  i = (*idx) + 1;
  j = 0;
  do
    {
      current = CFStringGetCharacterFromInlineBuffer (src, i++);
      tmp = (UInt8)((current > CHAR_A
        ? current - CHAR_A : current - CHAR_ZERO) & 0x0F) << 4;
      
      current = CFStringGetCharacterFromInlineBuffer (src, i++);
      tmp |= (UInt8)((current > CHAR_A
        ? current - CHAR_A : current - CHAR_ZERO) & 0x0F);
      
      bytes[j++] = tmp;
    } while (current == CHAR_PERCENT && i < 6);
  
  c = 0;
  str = bytes;
  num = GSStringEncodingToUnicode (enc, &c, 1, (const char**)&str, j,
    false, NULL);
  if (num)
    (*idx) += (CFIndex)(str - bytes) + num;
  return c;
}

CFStringRef
CFURLCreateStringByReplacingPercentEscapesUsingEncoding (CFAllocatorRef alloc,
  CFStringRef origString, CFStringRef leaveEscaped, CFStringEncoding encoding)
{
  CFStringInlineBuffer iBuffer;
  CFStringRef ret;
  CFIndex sLength;
  CFIndex idx;
  UniChar *dst;
  UniChar *dpos;
  UniChar c;
  
  sLength = CFStringGetLength (origString);
  CFStringInitInlineBuffer (origString, &iBuffer, CFRangeMake (0, sLength));
  
  dst = CFAllocatorAllocate (alloc, sizeof(UniChar) * sLength, 0);
  dpos = dst;
  for (idx = 0 ; idx < sLength ; ++idx)
    {
      c = CFStringGetCharacterFromInlineBuffer (&iBuffer, idx);
      if (c == CHAR_PERCENT && leaveEscaped && (idx + 2) < sLength)
        {
          UniChar repChar;
          
          repChar = CFURLCharacterForPercentEscape (&iBuffer, &idx, encoding);
          if (CFURLStringContainsCharacter(leaveEscaped, repChar))
            // Skip the '%'
            (*dpos++) = c;
          else
            (*dpos++) = repChar;
        }
      else
        {
          (*dpos++) = c;
        }
    }
  
  ret = CFStringCreateWithCharacters (alloc, dst, (CFIndex)(dpos - dst));
  CFAllocatorDeallocate (alloc, dst);
  
  return ret;
}

Boolean
CFURLGetFileSystemRepresentation (CFURLRef url, Boolean resolveAgainstBase,
  UInt8 *buffer, CFIndex bufLen)
{
  return false;
}

CFStringRef
CFURLGetString (CFURLRef url)
{
  return url->_urlString;
}

CFURLRef
CFURLGetBaseURL (CFURLRef url)
{
  return url->_baseURL;
}

CFIndex
CFURLGetBytes (CFURLRef url, UInt8 *buffer, CFIndex bufLen)
{
  return 0;
}

CFRange
CFURLGetByteRangeForComponent (CFURLRef url, CFURLComponentType comp,
  CFRange *rangeIncludingSeparators)
{
  return CFRangeMake (kCFNotFound, 0);
}

Boolean
CFURLResourceIsReachable (CFURLRef url, CFErrorRef *error)
{
  return false;
}

void
CFURLClearResourcePropertyCache (CFURLRef url)
{
  
}

void
CFURLClearResourcePropertyCacheForKey (CFURLRef url, CFStringRef key)
{
  
}

CFDictionaryRef
CFURLCopyResourcePropertiesForKeys (CFURLRef url, CFArrayRef keys,
  CFErrorRef *error)
{
  return NULL;
}

Boolean
CFURLCopyResourcePropertyForKey (CFURLRef url, CFStringRef key,
  void *propertyValueTypeRefPtr, CFErrorRef *error)
{
  return false;
}

CFDictionaryRef
CFURLCreateResourcePropertiesForKeysFromBookmarkData (CFAllocatorRef alloc,
  CFArrayRef resourcePropertiesToReturn, CFDataRef bookmark)
{
  return NULL; // FIXME: ???
}

CFTypeRef
CFURLCreateResourcePropertyForKeyFromBookmarkData (CFAllocatorRef alloc,
  CFStringRef resourcePropertyKey, CFDataRef bookmark)
{
  return NULL; // FIXME: ???
}

Boolean
CFURLSetResourcePropertiesForKeys (CFURLRef url,
  CFDictionaryRef keyedPropertyValues, CFErrorRef *error)
{
  return false;
}

Boolean
CFURLSetResourcePropertyForKey (CFURLRef url, CFStringRef key,
  CFTypeRef propertValue, CFErrorRef *error)
{
  return false;
}

void
CFURLSetTemporaryResourcePropertyForKey (CFURLRef url, CFStringRef key,
  CFTypeRef propertyValue)
{
  
}

CFDataRef
CFURLCreateBookmarkData (CFAllocatorRef alloc, CFURLRef url,
  CFURLBookmarkCreationOptions options, CFArrayRef resourcePropertiesToInclude,
  CFURLRef relativeToURL, CFErrorRef *error)
{
  return NULL; // FIXME: ???
}

CFDataRef
CFURLCreateBookmarkDataFromAliasRecord (CFAllocatorRef alloc,
  CFDataRef aliasRecordDataRef)
{
  return NULL; // FIXME: ???
}

CFDataRef
CFURLCreateBookmarkDataFromFile (CFAllocatorRef alloc, CFURLRef fileURL,
  CFErrorRef *errorRef)
{
  return NULL; // FIXME: ???
}

Boolean
CFURLWriteBookmarkDataToFile (CFDataRef bookmarkRef, CFURLRef fileURL,
  CFURLBookmarkFileCreationOptions options, CFErrorRef *errorRef)
{
  return false; // FIXME: ???
}