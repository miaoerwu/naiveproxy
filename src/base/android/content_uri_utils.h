// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ANDROID_CONTENT_URI_UTILS_H_
#define BASE_ANDROID_CONTENT_URI_UTILS_H_

#include <jni.h>
#include <string>

#include "base/base_export.h"
#include "base/files/file.h"
#include "base/files/file_path.h"

namespace base {

// Translates base::File::FLAG_* `open_flags` bitset to Java mode from
// ParcelFileDescriptor#parseMode(): ("r", "w", "wt", "wa", "rw" or "rwt").
// Disallows "w" which has been the source of android security issues.
// Returns nullopt if `open_flags` are not supported.
BASE_EXPORT std::optional<std::string> TranslateOpenFlagsToJavaMode(
    uint32_t open_flags);

// Opens a content URI and returns the file descriptor to the caller.
// `open_flags` is a bitmap of base::File::FLAG_* values.
// Returns -1 if the URI is invalid.
BASE_EXPORT File OpenContentUri(const FilePath& content_uri,
                                uint32_t open_flags);

// Gets file size, or -1 if file is unknown length.
BASE_EXPORT int64_t GetContentUriFileSize(const FilePath& content_uri);

// Check whether a content URI exists.
BASE_EXPORT bool ContentUriExists(const FilePath& content_uri);

// Gets MIME type from a content URI. Returns an empty string if the URI is
// invalid.
BASE_EXPORT std::string GetContentUriMimeType(const FilePath& content_uri);

// Gets the display name from a content URI. Returns true if the name was found.
BASE_EXPORT bool MaybeGetFileDisplayName(const FilePath& content_uri,
                                         std::u16string* file_display_name);

// Deletes a content URI.
BASE_EXPORT bool DeleteContentUri(const FilePath& content_uri);

}  // namespace base

#endif  // BASE_ANDROID_CONTENT_URI_UTILS_H_
