#!/bin/sh

set -e

rootdir="$(CDPATH='' cd "$(dirname "$0")/.." && pwd)"
licensefile="${rootdir}/LICENSE"
licensehead="$(sed '/^- /,$d' "${licensefile}")"
tmplicense="${rootdir}/~LICENSE.$$"
echo "$licensehead" > "$tmplicense"


# addlicense <library> <location> <license text>
addlicense() {
  licenseTextTrimmed=$(echo "$3" | sed -e 's/^/    /' -e 's/^    $//' -e 's/ *$//' | sed -e '/./,$!d' | sed -e '/^$/N;/^\n$/D')

  echo "
- ${1}, located at ${2}, is licensed as follows:
  \"\"\"
${licenseTextTrimmed}
  \"\"\"\
" >> "$tmplicense"

}


if ! [ -d "${rootdir}/deps/icu/" ] && ! [ -d "${rootdir}/deps/icu-small/" ]; then
  echo "ICU not installed, run configure to download it, e.g. ./configure --with-intl=small-icu --download=icu"
  exit 1
fi


# Dependencies bundled in distributions
licenseText="$(cat "${rootdir}"/deps/acorn/acorn/LICENSE)"
addlicense "Acorn" "deps/acorn" "$licenseText"
licenseText="$(tail -n +3 "${rootdir}"/deps/cares/LICENSE.md)"
addlicense "c-ares" "deps/cares" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/cjs-module-lexer/LICENSE)"
addlicense "cjs-module-lexer" "deps/cjs-module-lexer" "$licenseText"
if [ -f "${rootdir}/deps/icu/LICENSE" ]; then
  # ICU 57 and following. Drop the BOM
  licenseText="$(sed -e '1s/^[^a-zA-Z ]*ICU/ICU/' -e :a -e 's/<[^>]*>//g;s/	/ /g;s/ +$//;/</N;//ba' "${rootdir}"/deps/icu/LICENSE)"
  addlicense "ICU" "deps/icu" "$licenseText"
elif [ -f "${rootdir}/deps/icu/license.html" ]; then
  # ICU 56 and prior
  licenseText="$(sed -e '1,/ICU License - ICU 1\.8\.1 and later/d' -e :a -e 's/<[^>]*>//g;s/	/ /g;s/ +$//;/</N;//ba' "${rootdir}"/deps/icu/license.html)"
  addlicense "ICU" "deps/icu" "$licenseText"
elif [ -f "${rootdir}/deps/icu-small/LICENSE" ]; then
  # ICU 57 and following. Drop the BOM
  licenseText="$(sed -e '1s/^[^a-zA-Z ]*ICU/ICU/' -e :a -e 's/<[^>]*>//g;s/	/ /g;s/ +$//;/</N;//ba' "${rootdir}"/deps/icu-small/LICENSE)"
  addlicense "ICU" "deps/icu-small" "$licenseText"
elif [ -f "${rootdir}/deps/icu-small/license.html" ]; then
  # ICU 56 and prior
  licenseText="$(sed -e '1,/ICU License - ICU 1\.8\.1 and later/d' -e :a -e 's/<[^>]*>//g;s/	/ /g;s/ +$//;/</N;//ba' "${rootdir}"/deps/icu-small/license.html)"
  addlicense "ICU" "deps/icu-small" "$licenseText"
else
  echo "Could not find an ICU license file."
  exit 1
fi

licenseText="$(cat "${rootdir}"/deps/uv/LICENSE)"
addlicense "libuv" "deps/uv" "$licenseText"
licenseText="$(cat deps/llhttp/LICENSE-MIT)"
addlicense "llhttp" "deps/llhttp" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/corepack/LICENSE.md)"
addlicense "corepack" "deps/corepack" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/undici/LICENSE)"
addlicense "undici" "deps/undici" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/openssl/openssl/LICENSE)"
addlicense "OpenSSL" "deps/openssl" "$licenseText"
licenseText="$(curl -sL https://raw.githubusercontent.com/bestiejs/punycode.js/HEAD/LICENSE-MIT.txt)"
addlicense "Punycode.js" "lib/punycode.js" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/v8/LICENSE)"
addlicense "V8" "deps/v8" "$licenseText"
licenseText="$(sed -e '/You should have received a copy of the CC0/,$d' -e 's/^\/\* *//' -e 's/^ \* *//' deps/v8/src/third_party/siphash/halfsiphash.cc)"
addlicense "SipHash" "deps/v8/src/third_party/siphash" "$licenseText"
licenseText="$(sed -e '/The data format used by the zlib library/,$d' -e 's/^\/\* *//' -e 's/^ *//' "${rootdir}"/deps/zlib/zlib.h)"
addlicense "zlib" "deps/zlib" "$licenseText"

# npm
licenseText="$(cat "${rootdir}"/deps/npm/LICENSE)"
addlicense "npm" "deps/npm" "$licenseText"

# Build tools
licenseText="$(cat "${rootdir}"/tools/gyp/LICENSE)"
addlicense "GYP" "tools/gyp" "$licenseText"
licenseText="$(cat "${rootdir}"/tools/inspector_protocol/LICENSE)"
addlicense "inspector_protocol" "tools/inspector_protocol" "$licenseText"
licenseText="$(cat "${rootdir}"/tools/inspector_protocol/jinja2/LICENSE)"
addlicense "jinja2" "tools/inspector_protocol/jinja2" "$licenseText"
licenseText="$(cat "${rootdir}"/tools/inspector_protocol/markupsafe/LICENSE)"
addlicense "markupsafe" "tools/inspector_protocol/markupsafe" "$licenseText"

# Testing tools
licenseText="$(sed -e '/^$/,$d' -e 's/^#$//' -e 's/^# //' "${rootdir}"/tools/cpplint.py | tail -n +3)"
addlicense "cpplint.py" "tools/cpplint.py" "$licenseText"
licenseText="$(cat "${rootdir}"/tools/node_modules/eslint/LICENSE)"
addlicense "ESLint" "tools/node_modules/eslint" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/googletest/LICENSE)"
addlicense "gtest" "deps/googletest" "$licenseText"

# nghttp2
licenseText="$(cat "${rootdir}"/deps/nghttp2/COPYING)"
addlicense "nghttp2" "deps/nghttp2" "$licenseText"

# large_pages
licenseText="$(sed -e '/SPDX-License-Identifier/,$d' -e 's/^\/\///' "${rootdir}"/src/large_pages/node_large_page.h)"
addlicense "large_pages" "src/large_pages" "$licenseText"

# deep_freeze
licenseText="$(sed -e '/SPDX-License-Identifier/,$d' -e 's/^\/\///' "${rootdir}"/lib/internal/freeze_intrinsics.js)"
addlicense "caja" "lib/internal/freeze_intrinsics.js" "$licenseText"

# brotli
licenseText="$(cat "${rootdir}"/deps/brotli/LICENSE)"
addlicense "brotli" "deps/brotli" "$licenseText"

licenseText="$(cat "${rootdir}"/deps/histogram/LICENSE.txt)"
addlicense "HdrHistogram" "deps/histogram" "$licenseText"

licenseText="$(curl -sL https://raw.githubusercontent.com/highlightjs/highlight.js/63f367c46f2eeb6f9b7a3545e325eeeb917f9942/LICENSE)"
addlicense "highlight.js" "doc/api_assets/highlight.pack.js" "$licenseText"

licenseText="$(curl -sL https://raw.githubusercontent.com/bnoordhuis/node-heapdump/0ca52441e46241ffbea56a389e2856ec01c48c97/LICENSE)"
addlicense "node-heapdump" "src/heap_utils.cc" "$licenseText"

licenseText="$(curl -sL https://raw.githubusercontent.com/isaacs/rimraf/0e365ac4e4d64a25aa2a3cc026348f13410210e1/LICENSE)"
addlicense "rimraf" "lib/internal/fs/rimraf.js" "$licenseText"

licenseText="$(cat "${rootdir}"/deps/uvwasi/LICENSE)"
addlicense "uvwasi" "deps/uvwasi" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/ngtcp2/LICENSE_ngtcp2)"
addlicense "ngtcp2" "deps/ngtcp2/ngtcp2/" "$licenseText"
licenseText="$(cat "${rootdir}"/deps/ngtcp2/LICENSE_nghttp3)"
addlicense "nghttp3" "deps/ngtcp2/nghttp3/" "$licenseText"

licenseText="$(curl -sL https://raw.githubusercontent.com/jprichardson/node-fs-extra/b34da2762a4865b025cac06d02d6a2f1f1027b65/LICENSE)"
addlicense "node-fs-extra" "lib/internal/fs/cp" "$licenseText"

licenseText="$(cat "${rootdir}"/deps/base64/base64/LICENSE)"
addlicense "base64" "deps/base64/base64/" "$licenseText"

mv "$tmplicense" "$licensefile"
