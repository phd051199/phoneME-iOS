# phoneME Core FastVM Optimization TODO

Goal: keep phoneME's mature J2ME/MIDP compatibility while moving the VM hot
path toward FastVM's lower-memory, lower-allocation, verifier-driven execution
architecture. Every phase must preserve a compatibility fallback until the new
path passes the real-game regression corpus.

## Current verified state

- Compact slot SoA is active for interpreter locals/operand stacks: `u64 payload[] + u8 kind[]`.
- Real-game allocation tracing now covers NinjaSchoolOffline. Lazy `Hashtable` backing removed constructor-only array churn; homogeneous raw array payload reduced traced array bytes from roughly **110.8 MB -> 57.9 MB (~48%)** on a comparable 15 s run.
- A JIT monitor correctness bug was isolated by JIT-on/off A/B: compiled monitor methods could generate roughly **940,250 `IllegalMonitorStateException` objects / 10 s** in NinjaSchoolOffline. BaselineJit now keeps methods containing `monitorenter`/`monitorexit` in the interpreter; JIT remains enabled for other methods, object allocation returned to the ~200k/10 s range and allocation failures returned to zero.
- Inline `OperandStack` object footprint: **312 -> 240 bytes** (~23% smaller).
- Inline `LocalVariables` object footprint: **384 -> 208 bytes** (~46% smaller; first SoA step was 296 bytes).
- Dynamic logical value payload: **16 bytes/value -> ~9 bytes/value** before vector bookkeeping; the former per-local reference index/position vectors are gone entirely.
- Typed `int/long/float/double/reference` push/pop/get/set paths are wired into normal and `wide` constant/load/store bytecodes plus primitive ALU/conversion/compare paths.
- GC root enumeration and JIT physical-frame export read compact storage directly.
- `bash Core/Tools/test-host.sh`: **PASS**.
- `PHONEME_SANITIZE=1 bash Core/Tools/test-host.sh`: **PASS** (ASan/UBSan).
- `bash Core/Tools/test-performance-host.sh ...`: **PASS** with Release LTO core build.
- Native/HLE corpus profiling before String intrinsics: **44,813 native calls / 2,083 registry lookups (~4.6%)**; the hottest entries were `String.length()` (20,027), `String.valueOf(int)` (20,001) and `String.charAt(int)` (433).
- After the String interpreter/JIT intrinsics and combined text allocation: **4,353 native calls** on the same host corpus (**~90.3% fewer**); `String.length()`, `String.valueOf(int)` and `String.charAt(int)` no longer appear in the native hot list.
- Metadata composite-key construction work on the same profiled host corpus fell **176,863 -> 14,734 -> 3,453** after zero-copy native signatures plus structured heterogeneous method/field/assignability cache keys (**~98.0% fewer** total). Hot cache hits no longer allocate concatenated key strings; owning strings are copied only on cold cache insertion.
- Class-repository cache hits fell **30,761 -> 8,431** (**~72.6% fewer**) after internal-name `string_view` lookup plus caching class instantiability in `ClassLayout`; repeated Java object/String allocation no longer reloads class metadata only to re-check interface/abstract flags. Class-cache misses remain 967 on the same corpus.
- Combined initialized-object allocation copies cached default fields under the allocation lock instead of `allocate + N set_field`; that first step cut public heap-lock operations **163,601 -> 159,192** (**4,409 fewer / ~2.7%**). Removing interpreter/JIT double-locks, bulk-copying network receive data into Java byte arrays, moving the last JIT checked array store onto the execution-gate VM-fast accessor, bulk/snapshotting measured M3G state, snapshotting LCDUI/Game API state, batching `HashMap`/`Hashtable`/`ArrayList`/`ArrayDeque`/`HashSet`/`Calendar` state, batching byte-array and GZIP streams, bulk-serializing `Properties.store`, reusing IO runtime-class dispatch, and mutating `StringBuilder`/`StringBuffer` text through one heap transaction brought the latest stable profiled host corpus to **44,993** public locked operations total (**118,608 fewer / ~72.5% fewer vs 163,601**). The current snapshot passes full host profiling plus ASan/UBSan through profile/sanitizer 33. Host allocation-heavy fixtures vary between runs, so subsystem/domain deltas are used for attribution rather than treating stress-fixture total-count movement as a regression.
- The latest measured subsystem passes include: `StringBuilder`/`StringBuffer` heap transactions (**java.lang locks -14.7%** on that batch), `Hashtable` state/bucket snapshots (**java.util 5,143 -> 5,037**), stream class-dispatch/array metadata reuse (**IO 3,488 -> 3,283 / ~5.9% fewer**), Sprite/TiledLayer snapshots plus Choice/List bulk event state (**LCDUI 4,973 -> 3,948 before the command pass**), and LCDUI command sorting with one `{type, priority}` snapshot per command (**LCDUI 3,948 -> 3,756**). The M3G render traversal/state batch also reduced `Graphics3D`-attributed locks **2,865 -> 2,783** while keeping graphics VM tests green.
- Allocation tracing showed the apparent `other_java` allocation hotspot is dominated by the intentional `corefixture/GcOps` stress fixture (`GcOps -> java/lang/String`: **20,002** traced allocations), not representative game/runtime churn. A global lock-free VM allocator experiment was therefore rejected after it changed execution/exception behavior despite reducing allocation locks; public allocator locking remains in place until a stronger concurrency/ownership design is proven safe.
- Native network bulk receive no longer writes each received byte through `Heap::set_element`: the 64 KiB `DataInputStream.readFully` regression now uses one `write_byte_array` per received chunk. Profiled `DataInputStream`-attributed heap locks fell **65,954 -> 422 (~99.4%)** while the 64 KiB byte-for-byte socket fixture remains green.
- JIT runtime `array_store` now uses `vm_set_element_checked` under the execution gate instead of reacquiring the public heap mutex. Profiled checked array-store locks fell **11,709 -> 0** with kind/bounds/reference-assignability checks preserved.
- M3G render/scene array extraction now uses one-lock primitive-array bulk reads/writes instead of `Heap::element/set_element` per value. Profiled 3D heap locks fell **15,763 -> 9,233 (~41.4%)** after this pass; `Graphics3D`-attributed locks fell **8,652 -> 5,735** and scene/other M3G locks **5,087 -> 2,093**. Shared multi-field snapshots for `VertexArray`, `VertexBuffer`, `Image2D`, `Texture2D`, `Transformable`, `AnimationTrack`, `AnimationController` and `KeyframeSequence` then brought 3D locks to **8,251**, 3D field locks **5,007 -> 4,025**, and scene/other M3G locks to **1,936**. Targeted M3G, full profiled host and ASan+UBSan suites pass on this snapshot.
- LCDUI bridge-event construction now snapshots fixed field groups under one heap lock for screen/Alert, Item, TextField/Gauge/DateField/ImageItem, item-style, TextBox and Command events. Profiled LCDUI heap locks fell **12,515 -> 12,094**, with LCDUI field locks **4,282 -> 3,861 (~9.8% fewer)**. LCDUI extended, full profiled host and ASan+UBSan suites pass on this snapshot.
- VM-internal class checks now borrow the heap object's stable class-name storage while the execution gate is held instead of copying `std::string` values for virtual/interface dispatch, `aastore`, `checkcast`/`instanceof`, exception dispatch and measured intrinsics. The old owning `vm_class_name()` fast accessor has no callers and was removed.
- Direct execution of the already-built Release LTO `vm-performance-tests` binary over 10 consecutive runs is stable at **41.46 ms median** (40.87-42.28 ms range). Slower 61-121 ms samples came from repeatedly invoking the build wrapper and are treated as compile/thermal noise, not VM regression.
- Root-publication profiling is now explicit. On the current full host corpus the VM publishes execution roots **51,412 times / 102,346 ObjectRefs total** (max **47** roots/publication). Safepoint/native/monitor/quantum publication now swaps a reusable vector with `ExecutionContext` instead of rebuilding the roots and then copying them again with `vector::assign`; GC publication keeps the old copy path because the collector still consumes that same local root vector immediately afterward.
- Slot fallback counters show the same host corpus creates only **7** oversized frames: **7 local-storage fallbacks, 0 operand-stack fallbacks**, with maxima of **81 locals / 15 operand slots**. Keep the 16-slot inline operand bank; increasing it would enlarge every frame without helping this corpus.
- Interpreter safepoints now consume the verifier's immutable reference maps directly through an O(1) decoded-instruction -> verified-frame index. Full exact-map matches cover **2,445** frame scans and suspended/in-progress call states use a hybrid path for **4,226** more scans: locals remain exact while only the temporarily mismatched operand stack falls back to compact scanning. On the full host corpus this reduces root slot checks from an equivalent **75,158 -> 30,770 (~59.1% fewer)**, with **44,388 slot checks avoided** and no frame requiring a full locals+stack fallback. Published roots also fall **102,346 -> 102,044** because verifier-dead references left physically in local slots are no longer kept artificially live.

## Guardrails

- [x] Keep the existing phoneME J2ME APIs, LCDUI, media, RMS, network, M3G/Micro3D and host frontends.
- [x] Keep the ARM64 JIT; optimize the interpreter/JIT boundary instead of deleting JIT.
- [x] Do not change the 64-bit generation-checked `ObjectRef` during the early phases.
- [x] Preserve malformed/obfuscated-game compatibility through slow/fallback paths.
- [ ] Capture same-JAR baseline metrics before claiming a speed/thermal win.
- [ ] Keep host + sanitizer + iOS regression green after every migration step.

## Phase 0 — Baseline and observability

- [~] Add counters for interpreted calls, JIT calls, OSR entries/deopts and fallback reasons (method/native/opcode counters and JIT statistics already exist; consolidate A/B reporting still pending).
- [x] Add frame depth/high-water and oversized-frame counters: profiling records `maximum_java_call_depth`, `oversized_execution_frames`, local/operand fallback counts, maximum local/operand slots, frame-stack growths/capacity and pool misses.
- [x] Add slot-storage allocation/fallback counters.
- [x] Add root-publication/root-count counters sampled outside release hot paths.
- [ ] Record CPU time, frame pacing, allocations, wakeups and thermal trend on representative JARs.
- [ ] Store a reproducible A/B workload for NinjaSchoolOffline, Army2, Majesty and Zombie Infection.

## Phase 1 — Compact slot storage

- [x] Preserve the existing `Value` API at the Machine/JIT/native boundary.
- [x] Replace `OperandStack` internal `Value[]` storage with split `u64 payload[] + u8 kind[]` storage.
- [x] Replace `LocalVariables` internal `Value[]` storage with split `u64 payload[] + u8 kind[]` storage.
- [x] Scan reference roots directly from compact kind/payload storage without materializing `Value`.
- [x] Remove eager local-reference index/position bookkeeping: `astore`/overwrite no longer maintains secondary root vectors; coarse GC safepoints scan the compact local `kind[]` array instead, shrinking inline locals to 208 bytes and removing two overflow vectors.
- [x] Export JIT physical bits directly from compact storage.
- [x] Add direct typed `push_int/push_ref/pop_int/pop_ref` APIs for verified bytecodes.
- [x] Migrate hottest interpreter opcodes to typed/compact slot APIs so they stop constructing temporary `Value`s: constants, normal/wide loads/stores, primitive ALU/conversions/comparisons, stack shuffles and common object/array pushes are compact, and verified invoke argument popping now transfers `CompactStackValue -> InvocationArguments` directly. Remaining `Value` materialization is intentionally limited to heap/native/return and unverified compatibility boundaries.
- [x] Run JVM stack-shuffle opcodes (`pop/pop2/dup*`/`swap`) directly on compact `{kind,bits}` entries without materializing generic `Value` objects.
- [ ] Add compact storage microbenchmarks versus the legacy `Value` representation.
- [x] Record compact slot object/layout footprint against the prior `Value[16]` layout.

## Phase 2 — Reusable execution frames

- [x] Remove ordinary `invoke*` argument-vector allocation with an 8-Value inline argument buffer and explicit overflow fallback.
- [x] Stop copying/parsing `MethodDescriptor` state into every interpreted frame; retain the immutable cached descriptor by shared reference.
- [x] Construct interpreted frames directly in the reserved call-stack storage instead of creating/moving a temporary frame with inline slot banks.
- [x] Move the inline `invoke*` argument buffer directly into `Invocation` on the normal interpreter call path instead of copying it a second time in `prepare_invocation`.
- [x] Make inline invocation-buffer copy/move touch only live argument values; overflow signatures move their vector storage without copying payloads.
- [x] Stop zeroing unused inline operand/local payload banks on every frame construction; initialize only the active local kind/root metadata prefix.
- [x] Evaluate a fixed `ExecutionFrameBank` against profiling. Current corpus: 46,216 invocations, max Java depth 5, zero call-stack growth beyond reserved 32; keep the lighter pooled-vector design unless real-game profiles demonstrate deeper/hotter call stacks.
- [x] Reuse the reserved `std::vector<ExecutionFrame>` backing store across `execute()` calls with a nesting-safe thread-local pool, eliminating the repeated root call-stack allocation after warm-up.
- [x] Reuse in-vector frame object storage plus inline local/operand banks for ordinary Java calls.
- [x] Keep explicit vector-backed local/operand fallback for oversized/obfuscated methods and ordinary vector growth for call depth beyond the reserved 32-frame hot capacity.
- [x] Remove ordinary interpreted nested call/return heap allocation after warm-up for the hot profile (<=16 local/operand slots, <=8 argument values, <=32 depth).
- [x] Track call-depth/capacity, argument overflow and oversized-frame counts. Current profiled corpus: 26 argument overflows, 7 oversized frames, max locals 81, max operand stack 15.
- [x] Add profiling counters for invocation-argument overflow, oversized frame slots, call-stack growth/capacity and frame slot maxima so a bank decision is data-driven.
- [x] Verify pooled/reused frame behavior against existing exception-unwind, synchronized/monitor cleanup, legacy JSR/RET and native Java-exception recursion coverage; latest ASan+UBSan host suite reaches `Standalone Core tests passed`.

## Phase 3 — Verifier-driven quick execution

- [x] Produce immutable per-method slot/type metadata from the verifier: `RuntimeMethod` retains exact per-PC physical slot/reference maps; interpreted invoke fast paths use their presence as the verifier trust boundary and JIT/OSR/background compilation reuse the same maps.
- [~] Ensure field/method/class operands are quick-linked before steady-state execution. Field operands and invoke-site signatures are now quick-linked. Receiver class-name copies are gone via execution-gate-scoped borrowed views; storing `ClassId` directly in every heap object is intentionally deferred because the object is currently 120 bytes, a naive ID field grows it to ~128 bytes, and metadata clear/republication requires generation-safe identity.
- [x] Make `getfield/putfield/getstatic/putstatic` use direct IDs/slots in steady state: Machine-local CP-index slots now cache compact `FieldId + ClassId + slot + ValueKind` metadata, static storage is prepared once on cold resolution, `FieldId` static storage is a dense indexed vector instead of an `unordered_map`, and instance access goes directly through the resolved heap slot without retaining string-bearing `FieldLocation` objects.
- [~] Make invoke bytecodes use direct MethodId/vtable/signature slots only: every invoke site now caches its symbolic member reference plus parsed `CachedMethodDescriptor` once, resolved `MethodId` lookup is dense-array indexed in both Machine and RuntimeMetadata, and direct/virtual caches already retain compact method/native IDs. Interpreter and JIT receiver dispatch borrow the heap class-name storage without allocation/copy before the existing `ClassId` cache lookup; fully embedding generation-safe `ClassId` in object metadata remains deferred.
- [~] Replace decoded-off invoke fallback's second hash lookup with direct CP-index slots for static/special calls and sparse CP-index slots for polymorphic virtual/interface inline caches; the common invoke-site path also caches `owner/name/descriptor + CachedMethodDescriptor` so steady-state execution no longer reconstructs three constant-pool strings or hashes the descriptor on every call.
- [x] Remove repeated descriptor/type checks from verified hot paths: interpreted `invoke*`/`invokedynamic` skip caller-pop and `prepare_invocation` parameter-kind loops when immutable verifier metadata is present, verified arguments transfer compact kind/payload pairs directly, and external/unverified entry points retain checked validation.
- [x] Keep a checked fallback for unverifiable compatibility cases: unverified invoke argument popping still materializes and descriptor-checks each `Value`, `prepare_invocation` retains external/unverified validation, and exact GC maps fall back to compact kind scans when verifier metadata is unavailable/incompatible.

Current Phase 3 validation snapshot: strict C++23 compile with decoded execution OFF/ON **PASS**, full host suite **PASS** (`Standalone Core tests passed`), profiling+decoded full host suite **PASS**, targeted VM-dispatch/JIT/extended-opcode suites **PASS**, Release LTO performance suite **PASS**, and ASan+UBSan full host suite **PASS** after structured keys, borrowed class-name views, combined initialized-object allocation and cached class instantiability. Direct no-rebuild Release benchmark median is **41.46 ms** across 10 runs, within the existing ~40-42 ms band and treated as no regression rather than a measured speedup.

## Phase 4 — Compact native/HLE dispatch

- [x] Assign/cache compact native IDs during linkage: invoke-site/direct/virtual caches retain `NativeMethodId`, while `RuntimeMethod` now publishes a generation-checked atomic native/HLE ID after first linkage so subsequent invocation is lock-free; registry-generation changes invalidate by mismatch and refresh through the checked resolver.
- [x] Make repeated native calls dispatch by integer ID/function pointer only: decoded operand/direct/virtual caches retain `NativeMethodId + registry generation`, and `RuntimeMethod` now owns a generation-checked atomic native/HLE ID cache so steady-state `prepare_invocation` avoids `native_bindings_mutex_` entirely. Registry generation changes fall back to the checked resolver and republish the compact ID.
- [x] Remove per-native-call registry mutex and signature/`std::function` copies with an immutable ID-indexed dispatch snapshot; core bootstrap uses explicit registration batching so the table is published once instead of copied after every native registration, while late registrations remain immediately visible.
- [x] Remove the registry mutex and signature/`std::function` copies from `invoke(NativeMethodId)`: native entries are stable shared objects behind an immutable ID table published through one atomic raw pointer; snapshots remain owned for the registry lifetime, so the read path avoids even atomic `shared_ptr` refcount traffic while registry generation reads stay lock-free.
- [x] Remove composite native registry key allocation: registration stores heterogeneous `{owner,name,descriptor}` views into stable entry signatures, resolution hashes those views directly, and owner-level indexing rejects classes with no HLE/native registrations before full signature lookup. Machine runtime-method bindings use dense `MethodId` slots with generation invalidation; pointer hashing remains only as the compatibility fallback for methods outside RuntimeMetadata.
- [x] Keep native coverage off the call hot path: per-ID counters are merged into the optional process coverage report at registry teardown/clear instead of taking a coverage mutex/map lookup for every native call.
- [~] Audit `java.lang`, String, Math, System, LCDUI and Game API hot natives for string/hash lookup. The String-dominated first pass cut native calls 44,813 -> 4,353; direct zero-copy `String.equals(Object)` now removes another 172 native/method invocations in the current full fixture (4,316 -> 4,144 versus the pre-equals snapshot). `Object.<init>` bypass was tested and reverted because it changed stable linkage-error semantics; remaining work should follow real-game profiles rather than force unsafe micro-intrinsics.
- [x] Inline/rewrite the measured leaf String hot natives: interpreter/JIT execute final `String.length()`/`charAt(int)` directly against VM-fast heap accessors, and `String.valueOf(int)` formats/allocates directly while preserving fresh-object identity and progress-watchdog semantics.
- [x] Keep native-call hotness counters per compact `NativeMethodId`; reads/resets now operate on the immutable dispatch snapshot instead of taking the registry mutex on every invocation.

## Phase 5 — Interpreter/JIT unification

- [x] Define one compact physical-frame ABI shared by interpreter and BaselineJit: `JitPhysicalFrameView` carries BCI, local/stack physical slot counts and one raw `u64` span; interpreter OSR export and precise JIT deopt/restore now use the same validated layout. Normal method entry intentionally remains the smaller invocation-argument ABI because it is not a live execution frame.
- [x] Remove avoidable `Value` vector construction on JIT entry: Machine root/nested/JIT-to-JIT paths pass `InvocationArguments` directly, chained calls no longer build an intermediate `Value[]`, and BaselineJit now points the generated entry straight at the compact argument payload span after kind validation instead of copying it into a second `u64[32]`/overflow buffer.
- [x] Remove duplicate method resolution and heap-vector argument construction from JIT runtime-call fallback: resolved JIT calls now build the same <=8-value inline `InvocationArguments` used by the interpreter and enter `execute()` directly with verifier-trusted kinds.
- [x] Remove the remaining heap `vector<Value>` from JIT invoke operand decoding: runtime invoke dispatch decodes the physical caller stack straight into <=8-value compact `InvocationArguments`; JIT-to-JIT and interpreter fallback copy raw kind/payload pairs directly, while only the rare lambda-metafactory adaptation materializes `Value`s at its compatibility boundary.
- [x] Pack JIT entry arguments by validated `ValueKind + raw_bits_unchecked()` instead of repeating checked `as_*` conversions/bitcasts; do not clear unused entries in the 32-slot inline ABI buffer.
- [~] Remove avoidable frame reconstruction/copies on OSR (OSR physical-frame export writes directly into a 128-slot inline buffer, only allocates for unusually large frames, and now shares the exact `JitPhysicalFrameView` ABI with deopt restore; one unavoidable flattening copy remains because compact operand values must be expanded to physical JVM slots before the generated OSR entry loads them).
- [x] Remove background-JIT method-name/descriptor copies and re-lookups: queued work pins the immutable `ClassFile` plus stable `Method*`, retains shared descriptor/verifier metadata, and compiles/publishes directly from those identities.
- [x] Preserve proactive GC under allocation-heavy compiled HLE loops without publishing roots on every allocation: JIT `String.valueOf(int)` stages its precise caller roots, commits them only when the no-lock heap threshold says a collection is actually due, then performs GC before allocating. Ordinary text-object allocation never collects internally, so successful allocation-only calls stay scheduler-free; constrained 512 KiB pressure still completes through the guarded GC path while existing `new/newarray` verifier-root contracts remain untouched.
- [x] Restore compact frames directly on precise deopt: `JitDeoptState` carries one raw `u64` physical-slot vector (`locals + operand stack`) instead of `vector<optional<Value>> + vector<Value>`, and `ExecutionFrame` restores those bits straight into compact local/operand storage using the immutable verifier frame map. Category-2 continuation slots stay physical in the ABI and are skipped without materializing `Value` objects.
- [~] Reuse verifier type metadata in JIT lowering (reference/deopt frame maps now come from `RuntimeMethod` instead of rebuilding verifier state at each JIT/OSR/background compile; broader typed lowering still has room to consume more `VerifiedSlotKind` data).
- [ ] Tune JIT/OSR thresholds for minimum package power at target FPS, not maximum synthetic throughput.
- [x] Keep interpreter-only mode efficient when iOS JIT entitlement is unavailable: root/nested JIT admission analysis is skipped when `jit_.enabled()==false`, and OSR backedge accounting/entry work is gated off before `note_osr_backedge()` so interpreter-only execution pays no recurring JIT-analysis tax.

## Phase 6 — GC/root hot-path optimization

- [x] Give every verified frame an exact reference-slot map/bitmap (`RuntimeMethod` owns exact per-PC maps plus an O(1) decoded-instruction index, and interpreter safepoints now select them directly with a stack-depth compatibility guard).
- [x] Enumerate roots without building temporary `Value`s (exact verifier-map reads and compact fallback scans both consume kind/payload storage directly).
- [x] Reduce temporary root-vector growth/copies at safepoints: interpreter and JIT runtime safepoints both use reusable two-buffer exchange publication, eliminating the second full root-list copy; initial invocation and GC collection deliberately retain copy semantics because those local root vectors remain live after publication.
- [x] Keep nested JIT root ownership exact: root exchange returns a read-only view of the newly published buffer for JIT-to-JIT chaining, and synthetic child root depths are RAII-cleared when a fast chained callee returns so stale references cannot remain pinned after the child activation ends.
- [~] Publish roots only at required safepoints/native transitions: profiled `invoke_static`/`invoke_virtual` stage only a lazy `frame_base + reference-offset map` and commit to `ExecutionContext` only before real GC or recursive/blocking interpreter entry. Baseline publication traffic fell **51,380 -> ~7,775 (~84.9%)** before later String intrinsic noise; 43,618 invokes stage but only 13 commit. Lazy materialization means only 17/43,618 staged snapshots are flattened (0.039%), and only **15 / 63,624 deferred reference slots (0.0236%)** are actually read. `invoke_special/interface` staging was measured and reverted because 276/281 staged calls immediately committed, saving only 5 publications while adding work.
- [x] Audit allocation retry and native-root coverage under host ASan/UBSan: the current full core snapshot (lazy deferred roots/reference maps, compact verified invoke transfer, generation-checked RuntimeMethod native-ID cache and zero-copy String.equals intrinsic) passes official monolithic decoded-ON ASan+UBSan and official decoded-OFF host tests. The earlier `FakeNetworkAdapter/std::function` invalid-vptr report came from a manually mixed static-library/test-TU build and does not reproduce under the official same-snapshot sanitizer build.
- [ ] Measure GC pause time and roots scanned per frame.

## Phase 7 — Heap/object layout (only after profiling)

- [x] Add combined text-object allocation for String/StringBuilder-style payloads: class field defaults and UTF-16 payload are installed under one heap lock/allocation accounting pass; native-created and interned Strings no longer allocate an empty object and then attach text under a second lock.
- [ ] Measure whether 64-bit generation-checked `ObjectRef` is materially expensive.
- [ ] Benchmark compact object/array headers without changing reference semantics first.
- [ ] Reduce per-object `Value` field/array storage where verifier/class layout permits typed storage.
- [ ] Avoid a 32-bit handle migration unless measured gains justify GC/JIT/native compatibility risk.


## Phase 9 — JarlyME/Harrier thermal convergence

- [x] Reverse-engineer the local `JarlyME-0.5.ipa` execution model: RoboVM AOT host, Harrier guest interpreter, preallocated primitive/reference frame banks, quick-linked metadata, compact HLE IDs, coarse scheduling and no guest JIT/OSR machinery in the steady-state interpreter.
- [x] Remove physical-iOS native helper fan-out by default (`PHONEME_NATIVE_WORKERS` remains an explicit diagnostic override); classic 240x320 frame conversion no longer wakes caller + two helper CPU lanes.
- [x] Make physical-iOS baseline JIT thermal-first: no background JIT, OSR, aggressive JIT-to-JIT chaining or threshold=1 loop/profile forcing; raise warm/startup thresholds while retaining method-entry JIT for genuinely hot code.
- [x] Reject JIT compilation for methods containing `monitorenter`/`monitorexit` until monitor/deopt ownership is proven correct. NinjaSchoolOffline A/B removed a ~940k/10s `IllegalMonitorStateException` storm while keeping the rest of JIT enabled.
- [x] Lazy-materialize `java/util/Hashtable` backing arrays. Real-game tracing cut array-allocation count roughly in half because empty/short-lived tables no longer allocate five Java arrays in their constructor.
- [x] Replace Java array `vector<Value>` payload with homogeneous raw `u64` slots so array type is stored once in object metadata. NinjaSchoolOffline tracing reduced array payload bytes by about 48% before narrower primitive packing.
- [x] Pack Java arrays into one homogeneous raw payload at their natural widths: `[B`/`[Z` = 1 byte, `[C`/`[S` = 2 bytes, `[I`/`[F` = 4 bytes, `[J`/`[D`/references = 8 bytes. The optimizing JIT now uses 4-byte leases/loads/stores for int/float arrays while byte/boolean stays on the signedness-safe runtime path. CLDC, Release performance/JIT and full CoreTests pass with this representation.
- [x] Evaluate reclaimed small-array payload recycling and reject it for production: NinjaSchoolOffline measured only 14 hits vs 101,507 misses (~0.014% hit rate, 0.4 KiB reused), so the pool was removed instead of retaining RAM for negligible benefit.
- [x] Compact ordinary object fields into one aligned raw-u64 vector plus 3-bit packed `ValueKind` metadata in trailing words. This keeps the Object header and allocation count unchanged while moving multi-field objects toward ~8.4 bytes/field instead of 16; NinjaSchoolOffline measured `k23` at roughly 372 -> 260 bytes/object (~30% smaller) with Release throughput unchanged.
- [ ] Add a compact class/runtime ID directly to heap objects only if it can be done without growing the hot object header enough to erase lookup savings.
- [~] Move interpreter scheduling toward Harrier-style coarse safepoints: maintenance checks are already deadline-based instead of reading host foreground state every bytecode; retain only bounded quantum/fairness checks required for Java threading semantics.
- [x] Add physical-iOS sustained non-render spin throttling for worker loops that execute many quanta without producing a recent frame, with immediate reset at frame/input/blocking boundaries. The scheduler now expires stale frame activity instead of permanently exempting any thread that rendered once, accumulates uninterrupted active CPU time, and applies thermal-state-adaptive duty-cycle backoff only on physical iOS.
- [~] Make host polling demand-driven. Native LCDUI already backs off when idle; Canvas idle backoff is implemented and input/new frames force active cadence immediately; iOS build/device wakeup validation is pending.
- [~] Remove remaining CPU ARGB->RGBA channel shuffle on physical iOS with an explicit framebuffer pixel-format contract and direct BGRA Metal upload while preserving RGBA fallback/web APIs. Core now publishes little-endian ARGB rows as BGRA8 by memcpy and the Metal presentation path consumes `.bgra8Unorm`; physical iOS build/device validation is pending.
- [ ] Measure renderer upload/conversion CPU time, generated-vs-presented frame waste, host wakeups/sec, scheduler sleeps/yields and JIT compile time in the same real-game harness.
- [ ] Build current-source JIT-on and interpreter-only TrollStore TIPAs for device A/B after each thermal batch; compare FPS p95/p99, battery drain and thermal state before enabling any more aggressive parallelism/JIT.

## Phase 8 — Production A/B acceptance

- [ ] Run the same JAR, scene, resolution and FPS cap on baseline and optimized cores.
- [ ] Compare CPU time/frame and total CPU time.
- [ ] Compare allocations/sec and VM heap churn.
- [ ] Compare frame-time p50/p95/p99 and hitch count.
- [ ] Compare host wakeups/sec.
- [ ] Compare JIT compile time, OSR hit rate and deopt rate.
- [ ] Compare GC count/pause time.
- [ ] Compare device thermal state/package-power trend.
- [ ] Reject optimizations that improve synthetic throughput but increase energy at the target frame rate.
- [ ] Remove legacy slot/frame fallback only after the real-game corpus proves it unnecessary.

## Current next actions

- [x] Run the full host regression and sanitizer suite with compact slot storage.
- [x] Fix every regression before migrating interpreter opcodes to typed APIs.
- [x] Add typed stack/local accessors and migrate integer/reference load/store/constant opcodes first.
- [x] Measure before/after slot-storage object footprint.
- [ ] Measure before/after interpreter CPU on a deterministic VM fixture using a clean pre-optimization A/B build.
- [x] Audit current interpreted-call allocation behavior before introducing a full FastVM-style frame bank: the full corpus reports zero frame-stack growth, only 38 frame-pool misses, zero operand-slot fallback, 7 oversized/local-slot fallbacks and 26 >8-value argument overflows; normal verified invokes now use inline compact `InvocationArguments` without a per-call `vector<Value>`, so a global frame-bank rewrite is not justified by the measured profile.
- [x] Audit normal invoke setup: identified and removed the per-call `std::vector<Value>` allocation for <=8 receiver+parameter values.
