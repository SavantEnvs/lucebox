#!/usr/bin/env bash
#
# mayhem/build.sh — build lucebox's libFuzzer harnesses and the host-only unit suite.
#
# Upstream's CMake build requires CUDA or HIP (project() enables the GPU language; there is no
# CPU-only configure), so the host-only parser translation units under server/src are compiled
# directly here: the fuzzed sources with $SANITIZER_FLAGS $DEBUG_FLAGS, the oracle with normal flags.
# The only external dependency is nlohmann/json, from Debian's nlohmann-json3-dev (in the image).
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
# SanCov for the fuzzed objects (project TUs + harnesses): libFuzzer's coverage feedback needs the
# objects compiled with -fsanitize=fuzzer-no-link; the -fsanitize=fuzzer link alone instruments nothing.
: "${FUZZ_CFLAGS:=-fsanitize=fuzzer-no-link}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
S=server/src
OUT=mayhem/build            # gitignored by upstream's `build/` pattern
OBJ=$OUT/fuzz-obj
TOBJ=$OUT/test-obj
mkdir -p "$OBJ" "$TOBJ"

INC=(-I"$S" -I"$S/server" -I"$S/common" -I"$S/pflash" -Iserver/include
     -Iserver/deps/llama.cpp/common -Iserver/deps/llama.cpp/ggml/include)
CXXSTD=(-std=c++17 -pthread)

# Project sources each harness needs (all host-only, no GPU headers).
declare -A SOURCES=(
  [fuzz_tool_parser]="server/tool_parser.cpp"
  [fuzz_gbnf]="common/gbnf.cpp"
  [fuzz_json_schema_grammar]="server/json_schema_grammar.cpp common/gbnf.cpp"
  [fuzz_cluster_protocol]="cluster/cluster_protocol.cpp"
  [fuzz_pflash_policy]="pflash/pflash_request_probe.cpp common/pflash_drafter_ipc.cpp common/backend_ipc.cpp"
  [fuzz_model_card]="server/model_card.cpp"
  [fuzz_chat_template]="server/chat_template.cpp ../deps/llama.cpp/common/jinja/lexer.cpp ../deps/llama.cpp/common/jinja/parser.cpp ../deps/llama.cpp/common/jinja/runtime.cpp ../deps/llama.cpp/common/jinja/value.cpp ../deps/llama.cpp/common/jinja/string.cpp ../deps/llama.cpp/common/jinja/caps.cpp ../deps/llama.cpp/common/unicode.cpp"
)

obj_name() { local n="${1#../}"; echo "${n//\//_}.o"; }

# 1) Sanitized project objects (compiled once, shared by the harnesses).
all_srcs=$(printf '%s\n' "${SOURCES[@]}" | tr ' ' '\n' | sort -u)
pids=()
for src in $all_srcs; do
  o="$OBJ/$(obj_name "$src")"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" -c "$S/$src" -o "$o" &
  pids+=($!)
  if [ "${#pids[@]}" -ge "$MAYHEM_JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for p in "${pids[@]}"; do wait "$p"; done

$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$OBJ/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"

# 2) Each harness twice: libFuzzer binary and run-once standalone reproducer.
for h in "${!SOURCES[@]}"; do
  objs=()
  for src in ${SOURCES[$h]}; do objs+=("$OBJ/$(obj_name "$src")"); done
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" -c "mayhem/harnesses/$h.cpp" -o "$OBJ/$h.o"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE \
      "$OBJ/$h.o" "${objs[@]}" "$OBJ/lsan_off.o" -o "$SRC/$h"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS \
      "$OBJ/standalone_main.o" "$OBJ/$h.o" "${objs[@]}" "$OBJ/lsan_off.o" -o "$SRC/$h-standalone"
done

# 2b) Tokenizer harnesses: server/src/server/tokenizer.cpp + the vendored ggml-base (gguf reader),
#    compiled straight from server/deps/llama.cpp/ggml (no CMake: its parent build needs CUDA/HIP).
G=server/deps/llama.cpp/ggml
GINC=(-I"$G/include" -I"$G/src")
GGML_C=(src/ggml.c src/ggml-alloc.c src/ggml-quants.c rocmfp4/rocmfp4.c rocmfpx/rocmfpx.c)
GGML_CXX=(src/ggml.cpp src/ggml-backend.cpp src/ggml-backend-meta.cpp src/ggml-opt.cpp src/ggml-threading.cpp src/gguf.cpp)
gobjs=(); gp=()
for src in "${GGML_C[@]}"; do
  o="$OBJ/ggml_${src//\//_}.o"; gobjs+=("$o")
  $CC -std=gnu11 $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${GINC[@]}" -D_GNU_SOURCE -DGGML_VERSION=\"lucebox\" -DGGML_COMMIT=\"vendored\" -c "$G/$src" -o "$o" & gp+=($!)
done
for src in "${GGML_CXX[@]}"; do
  o="$OBJ/ggml_${src//\//_}.o"; gobjs+=("$o")
  # -fno-sanitize=enum: gguf.cpp reads the on-disk type tag straight into `enum gguf_type` before
  # validating it (gguf.cpp:522), so UBSan's enum check fires on the first byte-flip of any GGUF and
  # would mask everything behind it. Only that one check is relaxed, only on vendored ggml.
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS -fno-sanitize=enum $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${GINC[@]}" -D_GNU_SOURCE -DGGML_VERSION=\"lucebox\" -DGGML_COMMIT=\"vendored\" -c "$G/$src" -o "$o" & gp+=($!)
done
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" "${GINC[@]}" -c "$S/server/tokenizer.cpp" -o "$OBJ/server_tokenizer.cpp.o" & gp+=($!)
for p in "${gp[@]}"; do wait "$p"; done
for h in fuzz_tokenizer fuzz_tokenizer_gguf; do
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" "${GINC[@]}" -Imayhem/harnesses -c "mayhem/harnesses/$h.cpp" -o "$OBJ/$h.o"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE \
      "$OBJ/$h.o" "$OBJ/server_tokenizer.cpp.o" "${gobjs[@]}" "$OBJ/lsan_off.o" -lm -o "$SRC/$h"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS \
      "$OBJ/standalone_main.o" "$OBJ/$h.o" "$OBJ/server_tokenizer.cpp.o" "${gobjs[@]}" "$OBJ/lsan_off.o" -lm -o "$SRC/$h-standalone"
done

# 2c) Request-body and streaming-reply harnesses: api_adapters / image_input (the HTTP request's
#     messages and image data URLs) and sse_emitter / server_status (the SSE reply and /status
#     JSON). api_adapters' token classifier links the tokenizer, so they reuse the ggml objects.
HTTP_SRCS=(server/api_adapters.cpp server/image_input.cpp server/sse_emitter.cpp server/tool_memory.cpp
           server/prompt_normalize.cpp server/response_error.cpp server/chat_codec.cpp)
hobjs=("$OBJ/$(obj_name server/tool_parser.cpp)"); hp=()
for src in ${SOURCES[fuzz_chat_template]}; do hobjs+=("$OBJ/$(obj_name "$src")"); done  # chat_codec -> chat_template + jinja
for src in "${HTTP_SRCS[@]}"; do
  o="$OBJ/$(obj_name "$src")"; hobjs+=("$o")
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" -c "$S/$src" -o "$o" & hp+=($!)
done
for p in "${hp[@]}"; do wait "$p"; done
for h in fuzz_api_adapters fuzz_sse_emitter; do
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" -c "mayhem/harnesses/$h.cpp" -o "$OBJ/$h.o"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE \
      "$OBJ/$h.o" "${hobjs[@]}" "$OBJ/server_tokenizer.cpp.o" "${gobjs[@]}" "$OBJ/lsan_off.o" -lm -o "$SRC/$h"
  $CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS \
      "$OBJ/standalone_main.o" "$OBJ/$h.o" "${hobjs[@]}" "$OBJ/server_tokenizer.cpp.o" "${gobjs[@]}" "$OBJ/lsan_off.o" -lm -o "$SRC/$h-standalone"
done

# 2d) Image decoder: common/vision/image_decode.cpp with libjpeg-turbo and lodepng, from the
#     sources the Dockerfile cached at upstream's ImageCodecs.cmake pins. The codecs are what parse
#     the untrusted bytes (decode_image is a thin wrapper), so they get the sanitizers and SanCov
#     too, built out of tree under mayhem/build with upstream's own libjpeg-turbo options.
: "${JPEG_SRC:=$(ls -d /opt/toolchains/deps/libjpeg-turbo-*/src | head -1)}"
: "${LODEPNG_SRC:=$(ls -d /opt/toolchains/deps/lodepng-*/src | head -1)}"
JB=$OUT/libjpeg-turbo
CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1" cmake -S "$JPEG_SRC" -B "$JB" -G Ninja \
    -DCMAKE_BUILD_TYPE=None -DCMAKE_INSTALL_PREFIX="$SRC/$JB/install" -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_SIMD=OFF \
    -DWITH_TURBOJPEG=OFF >/dev/null
cmake --build "$JB" --parallel "$MAYHEM_JOBS" >/dev/null
cmake --install "$JB" >/dev/null
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 -c "$LODEPNG_SRC/lodepng.cpp" -o "$OBJ/lodepng.o"
CODEC_INC=(-I"$JB/install/include" -I"$LODEPNG_SRC")
CODEC_LIBS=("$OBJ/lodepng.o" "$JB/install/lib/libjpeg.a")
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" "${CODEC_INC[@]}" \
    -c "$S/common/vision/image_decode.cpp" -o "$OBJ/common_vision_image_decode.cpp.o"
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS -O1 "${INC[@]}" \
    -c mayhem/harnesses/fuzz_image_decode.cpp -o "$OBJ/fuzz_image_decode.o"
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ/fuzz_image_decode.o" \
    "$OBJ/common_vision_image_decode.cpp.o" "${CODEC_LIBS[@]}" "$OBJ/lsan_off.o" -o "$SRC/fuzz_image_decode"
$CXX "${CXXSTD[@]}" $SANITIZER_FLAGS $DEBUG_FLAGS $FUZZ_CFLAGS "$OBJ/standalone_main.o" "$OBJ/fuzz_image_decode.o" \
    "$OBJ/common_vision_image_decode.cpp.o" "${CODEC_LIBS[@]}" "$OBJ/lsan_off.o" -o "$SRC/fuzz_image_decode-standalone"

# 3) Upstream's host-only unit tests (CppUnitTestFramework, as in test_server_unit) for the fuzzed
#    grammar stack and PFlash parsers, built with NORMAL flags for mayhem/test.sh.
TSRC=(common/gbnf.cpp common/output_constraint.cpp server/json_schema_grammar.cpp
      pflash/pflash_request_probe.cpp pflash/pflash_selection.cpp pflash/pflash_query_candidates.cpp
      common/pflash_drafter_ipc.cpp common/backend_ipc.cpp)
TTESTS=(test/unit/test_unit_main.cpp test/unit/test_gbnf.cpp
        test/pflash/test_pflash_drafter_ipc.cpp test/pflash/test_pflash_request_probe.cpp)
tobjs=(); tp=()
for src in "${TSRC[@]}"; do
  o="$TOBJ/${src//\//_}.o"; tobjs+=("$o")
  $CXX "${CXXSTD[@]}" -O1 $COVERAGE_FLAGS "${INC[@]}" -c "$S/$src" -o "$o" & tp+=($!)
done
for t in "${TTESTS[@]}"; do
  o="$TOBJ/$(basename "$t" .cpp).o"; tobjs+=("$o")
  $CXX "${CXXSTD[@]}" -O1 $COVERAGE_FLAGS "${INC[@]}" -Iserver/test -c "server/$t" -o "$o" & tp+=($!)
done
for p in "${tp[@]}"; do wait "$p"; done
$CXX "${CXXSTD[@]}" $COVERAGE_FLAGS "${tobjs[@]}" -o "$OUT/host_unit_tests"
echo "build.sh: done"
