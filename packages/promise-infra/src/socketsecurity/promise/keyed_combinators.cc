// node:smol-promise V8 binding — Promise.allKeyed / Promise.allSettledKeyed.
//
// Reading order, top to bottom: private-symbol slots, the promise-capability
// helpers, the per-key handlers, the shared driver, then registration.
// See keyed_combinators.h for what the module is and why the per-key state
// travels as a callback `data` object.

#include "keyed_combinators.h"
// The isolate comes from `Isolate::GetCurrent()`, never `context->GetIsolate()`:
// V8 removed that accessor, and no upstream Node source calls it any more. Every
// path here runs inside a V8 callback, so a current isolate is always entered.

#include "node.h"
#include "node_binding.h"
#include "node_external_reference.h"
#include "util.h"
#include "v8.h"

namespace node {
namespace socketsecurity {
namespace promise {

using v8::Array;
using v8::ConstructorBehavior;
using v8::Context;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Name;
using v8::Object;
using v8::Private;
using v8::PropertyFilter;
using v8::String;
using v8::TryCatch;
using v8::Value;

// ═══════════════════════════════════════════════════════════════════════
// Private-symbol slots.
// ═══════════════════════════════════════════════════════════════════════
//
// `Private::ForApi` interns one symbol per name per isolate, so these
// helpers can be called on every access without caching anything: the
// second call for a given name returns the same symbol as the first.
// A private symbol is not a property key from JS's point of view, so a
// dictionary that is a Proxy never sees these reads or writes.

// Combinator-wide record.
static Local<Private> SlotRemaining(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedRemaining"));
}

static Local<Private> SlotResult(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedResult"));
}

static Local<Private> SlotResolve(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedResolve"));
}

// Per-key record.
static Local<Private> SlotKey(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedKey"));
}

static Local<Private> SlotShared(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedShared"));
}

static Local<Private> SlotAlreadyCalled(Isolate* isolate) {
  return Private::ForApi(
      isolate, FIXED_ONE_BYTE_STRING(isolate, "smolKeyedAlreadyCalled"));
}

// Promise-capability record, populated by CapabilityExecutor.
static Local<Private> SlotCapabilityResolve(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedCapResolve"));
}

static Local<Private> SlotCapabilityReject(Isolate* isolate) {
  return Private::ForApi(isolate,
                         FIXED_ONE_BYTE_STRING(isolate, "smolKeyedCapReject"));
}

// ═══════════════════════════════════════════════════════════════════════
// Small utilities.
// ═══════════════════════════════════════════════════════════════════════

// A record object with a null prototype. Nothing inherited can intercept a
// slot write, and for the RESULT object specifically the null prototype is
// what makes a plain data write equivalent to the spec's
// CreateDataPropertyOrThrow: with no prototype there is no inherited setter
// to run, `__proto__` included.
static Local<Object> NullProtoObject(Isolate* isolate) {
  return Object::New(isolate, v8::Null(isolate), nullptr, nullptr, 0);
}

static void ThrowTypeError(Isolate* isolate, const char* message) {
  isolate->ThrowException(Exception::TypeError(
      String::NewFromUtf8(isolate, message).ToLocalChecked()));
}

// Read an int32 out of a private slot. The slot is always written before it
// is read, so a missing value would be a bug in this file rather than
// something a caller can provoke; 0 is the inert answer if it ever happens.
static int32_t ReadCount(Local<Context> context,
                         Local<Object> holder,
                         Local<Private> slot) {
  Local<Value> raw;
  if (!holder->GetPrivate(context, slot).ToLocal(&raw)) {
    return 0;
  }
  int32_t value = 0;
  if (!raw->Int32Value(context).To(&value)) {
    return 0;
  }
  return value;
}

static bool WriteCount(Local<Context> context,
                       Local<Object> holder,
                       Local<Private> slot,
                       int32_t value) {
  return holder
      ->SetPrivate(context, slot, Integer::New(Isolate::GetCurrent(), value))
      .IsJust();
}

// ═══════════════════════════════════════════════════════════════════════
// NewPromiseCapability.
// ═══════════════════════════════════════════════════════════════════════

// The executor passed to `new C(executor)`. Its `data` is the capability
// record, so the resolve / reject functions C hands us land in the record's
// private slots. Per GetCapabilitiesExecutor, being called twice is a
// TypeError — a constructor that invokes its executor more than once must
// not be able to overwrite an already-captured pair.
static void CapabilityExecutor(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> capability = args.Data().As<Object>();

  Local<Value> existing;
  if (!capability->GetPrivate(context, SlotCapabilityResolve(isolate))
           .ToLocal(&existing)) {
    return;
  }
  if (!existing->IsUndefined()) {
    ThrowTypeError(isolate,
                   "Promise capability executor was already invoked: a "
                   "constructor may call its executor only once");
    return;
  }

  Local<Value> resolve = args.Length() > 0 ? args[0] : v8::Undefined(isolate);
  Local<Value> reject = args.Length() > 1 ? args[1] : v8::Undefined(isolate);
  if (capability->SetPrivate(context, SlotCapabilityResolve(isolate), resolve)
          .IsNothing()) {
    return;
  }
  capability->SetPrivate(context, SlotCapabilityReject(isolate), reject)
      .IsJust();
}

// NewPromiseCapability(C). On success `out_capability` carries the resolve /
// reject pair and `out_promise` the constructed promise. On failure an
// exception is pending and false is returned.
//
// This runs BEFORE any TryCatch in the driver, which is deliberate: the
// spec spells step 2 as `? NewPromiseCapability(C)`, so a non-constructor
// `this` throws synchronously — there is no capability yet to reject
// through. Every later failure is routed to the capability's reject.
static bool NewPromiseCapability(Local<Context> context,
                                 Local<Value> constructor,
                                 Local<Object>* out_capability,
                                 Local<Value>* out_promise) {
  Isolate* isolate = Isolate::GetCurrent();
  if (!constructor->IsObject() || !constructor.As<Object>()->IsConstructor()) {
    ThrowTypeError(isolate,
                   "Promise keyed combinator called on a non-constructor: "
                   "`this` must be a constructor, e.g. Promise or a subclass");
    return false;
  }

  Local<Object> capability = NullProtoObject(isolate);
  if (capability
          ->SetPrivate(context,
                       SlotCapabilityResolve(isolate),
                       v8::Undefined(isolate))
          .IsNothing()) {
    return false;
  }
  if (capability
          ->SetPrivate(
              context, SlotCapabilityReject(isolate), v8::Undefined(isolate))
          .IsNothing()) {
    return false;
  }

  Local<Function> executor;
  if (!Function::New(context,
                     CapabilityExecutor,
                     capability,
                     2,
                     ConstructorBehavior::kThrow)
           .ToLocal(&executor)) {
    return false;
  }

  Local<Value> argv[] = {executor};
  Local<Value> promise;
  if (!constructor.As<Object>()
           ->CallAsConstructor(context, 1, argv)
           .ToLocal(&promise)) {
    return false;
  }

  Local<Value> resolve;
  Local<Value> reject;
  if (!capability->GetPrivate(context, SlotCapabilityResolve(isolate))
           .ToLocal(&resolve) ||
      !capability->GetPrivate(context, SlotCapabilityReject(isolate))
           .ToLocal(&reject)) {
    return false;
  }
  if (!resolve->IsFunction() || !reject->IsFunction()) {
    ThrowTypeError(isolate,
                   "Promise capability was not populated: the constructor's "
                   "executor must be called with two callable arguments");
    return false;
  }

  *out_capability = capability;
  *out_promise = promise;
  return true;
}

static Local<Function> CapabilityResolve(Local<Context> context,
                                        Local<Object> capability) {
  Isolate* isolate = Isolate::GetCurrent();
  return capability->GetPrivate(context, SlotCapabilityResolve(isolate))
      .ToLocalChecked()
      .As<Function>();
}

static Local<Function> CapabilityReject(Local<Context> context,
                                        Local<Object> capability) {
  Isolate* isolate = Isolate::GetCurrent();
  return capability->GetPrivate(context, SlotCapabilityReject(isolate))
      .ToLocalChecked()
      .As<Function>();
}

// ═══════════════════════════════════════════════════════════════════════
// Per-key handlers.
// ═══════════════════════════════════════════════════════════════════════

// Which combinator a handler belongs to, and for allSettledKeyed which of
// its two handlers is running.
enum class Outcome { kValue, kFulfilled, kRejected };

// Write one key's entry into the shared result object, then settle the
// combinator if this was the last outstanding key.
//
// The `alreadyCalled` flag lives on the per-key record, and for
// allSettledKeyed BOTH handlers get that same record. A thenable that calls
// its fulfill and its reject callback therefore decrements the counter once,
// which is the difference between resolving on time and resolving early
// with a half-filled result.
static void SettleElement(const FunctionCallbackInfo<Value>& args,
                          Outcome outcome) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  Local<Object> element = args.Data().As<Object>();

  Local<Value> already;
  if (!element->GetPrivate(context, SlotAlreadyCalled(isolate))
           .ToLocal(&already)) {
    return;
  }
  if (already->IsTrue()) {
    return;
  }
  if (element
          ->SetPrivate(
              context, SlotAlreadyCalled(isolate), v8::True(isolate))
          .IsNothing()) {
    return;
  }

  Local<Value> shared_raw;
  Local<Value> key_raw;
  if (!element->GetPrivate(context, SlotShared(isolate)).ToLocal(&shared_raw) ||
      !element->GetPrivate(context, SlotKey(isolate)).ToLocal(&key_raw)) {
    return;
  }
  Local<Object> shared = shared_raw.As<Object>();
  Local<Name> key = key_raw.As<Name>();

  Local<Value> settled = args.Length() > 0 ? args[0] : v8::Undefined(isolate);

  // allKeyed stores the value itself; allSettledKeyed stores an ordinary
  // `{ status, value }` / `{ status, reason }` record (ordinary, not
  // null-prototype: the spec builds it with OrdinaryObjectCreate).
  Local<Value> entry = settled;
  if (outcome != Outcome::kValue) {
    Local<Object> record = Object::New(isolate);
    Local<String> status = outcome == Outcome::kFulfilled
                               ? FIXED_ONE_BYTE_STRING(isolate, "fulfilled")
                               : FIXED_ONE_BYTE_STRING(isolate, "rejected");
    Local<String> field = outcome == Outcome::kFulfilled
                              ? FIXED_ONE_BYTE_STRING(isolate, "value")
                              : FIXED_ONE_BYTE_STRING(isolate, "reason");
    if (record
            ->CreateDataProperty(
                context, FIXED_ONE_BYTE_STRING(isolate, "status"), status)
            .IsNothing() ||
        record->CreateDataProperty(context, field, settled).IsNothing()) {
      return;
    }
    entry = record;
  }

  Local<Value> result_raw;
  if (!shared->GetPrivate(context, SlotResult(isolate)).ToLocal(&result_raw)) {
    return;
  }
  Local<Object> result = result_raw.As<Object>();
  if (result->CreateDataProperty(context, key, entry).IsNothing()) {
    return;
  }

  int32_t remaining = ReadCount(context, shared, SlotRemaining(isolate)) - 1;
  if (!WriteCount(context, shared, SlotRemaining(isolate), remaining)) {
    return;
  }
  if (remaining != 0) {
    return;
  }

  Local<Value> resolve_raw;
  if (!shared->GetPrivate(context, SlotResolve(isolate))
           .ToLocal(&resolve_raw)) {
    return;
  }
  Local<Value> resolve_argv[] = {result};
  USE(resolve_raw.As<Function>()->Call(
      context, v8::Undefined(isolate), 1, resolve_argv));
}

static void AllKeyedElement(const FunctionCallbackInfo<Value>& args) {
  SettleElement(args, Outcome::kValue);
}

static void AllSettledKeyedFulfillElement(
    const FunctionCallbackInfo<Value>& args) {
  SettleElement(args, Outcome::kFulfilled);
}

static void AllSettledKeyedRejectElement(
    const FunctionCallbackInfo<Value>& args) {
  SettleElement(args, Outcome::kRejected);
}

// ═══════════════════════════════════════════════════════════════════════
// The shared driver.
// ═══════════════════════════════════════════════════════════════════════

enum class Combinator { kAllKeyed, kAllSettledKeyed };

// Everything after NewPromiseCapability. Returns false with an exception
// pending on an abrupt completion; the caller turns that into a rejection.
static bool PerformKeyed(Local<Context> context,
                         Local<Value> constructor,
                         Local<Object> capability,
                         Local<Value> input,
                         Combinator kind) {
  Isolate* isolate = Isolate::GetCurrent();

  // GetPromiseResolve(C) — read `C.resolve` ONCE, before the key walk, and
  // reuse it for every key. Reading it per key would let a getter observe
  // the iteration.
  Local<Value> promise_resolve;
  if (!constructor.As<Object>()
           ->Get(context, FIXED_ONE_BYTE_STRING(isolate, "resolve"))
           .ToLocal(&promise_resolve)) {
    return false;
  }
  if (!promise_resolve->IsFunction()) {
    ThrowTypeError(isolate,
                   "Promise keyed combinator needs a callable `resolve`: "
                   "`this.resolve` is not a function");
    return false;
  }

  if (!input->IsObject()) {
    ThrowTypeError(isolate,
                   "Promise keyed combinator needs an object: the argument is "
                   "a dictionary of promises, not a primitive or an iterable");
    return false;
  }
  Local<Object> dictionary = input.As<Object>();

  // Own ENUMERABLE string keys, in [[OwnPropertyKeys]] order (integer-index
  // keys ascending, then string keys in insertion order). The
  // ONLY_ENUMERABLE filter IS the spec's per-key descriptor check, so an
  // inherited or non-enumerable key is skipped — and asking V8 for the
  // filtered set means a Proxy dictionary sees one ownKeys walk rather than
  // one walk plus a getOwnPropertyDescriptor trap per key.
  Local<Array> keys;
  if (!dictionary
           ->GetOwnPropertyNames(context,
                                 PropertyFilter::ONLY_ENUMERABLE,
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return false;
  }

  Local<Object> result = NullProtoObject(isolate);
  Local<Object> shared = NullProtoObject(isolate);
  if (shared->SetPrivate(context, SlotResult(isolate), result).IsNothing() ||
      shared
          ->SetPrivate(context,
                       SlotResolve(isolate),
                       CapabilityResolve(context, capability))
          .IsNothing()) {
    return false;
  }

  // The counter starts at 1, not 0, and the loop's own decrement happens
  // only after the walk ends. A dictionary of already-settled promises
  // would otherwise reach 0 on its first key and resolve the combinator
  // before the remaining keys had even been read.
  if (!WriteCount(context, shared, SlotRemaining(isolate), 1)) {
    return false;
  }

  Local<Function> reject = CapabilityReject(context, capability);

  for (uint32_t i = 0, length = keys->Length(); i < length; i += 1) {
    Local<Value> key_raw;
    if (!keys->Get(context, i).ToLocal(&key_raw)) {
      return false;
    }
    Local<Name> key = key_raw.As<Name>();

    Local<Value> value;
    if (!dictionary->Get(context, key).ToLocal(&value)) {
      return false;
    }

    Local<Value> resolve_argv[] = {value};
    Local<Value> next_promise;
    if (!promise_resolve.As<Function>()
             ->Call(context, constructor, 1, resolve_argv)
             .ToLocal(&next_promise)) {
      return false;
    }

    Local<Object> element = NullProtoObject(isolate);
    if (element->SetPrivate(context, SlotKey(isolate), key).IsNothing() ||
        element->SetPrivate(context, SlotShared(isolate), shared)
            .IsNothing() ||
        element
            ->SetPrivate(
                context, SlotAlreadyCalled(isolate), v8::False(isolate))
            .IsNothing()) {
      return false;
    }

    Local<Function> on_fulfilled;
    if (!Function::New(context,
                       kind == Combinator::kAllKeyed
                           ? AllKeyedElement
                           : AllSettledKeyedFulfillElement,
                       element,
                       1,
                       ConstructorBehavior::kThrow)
             .ToLocal(&on_fulfilled)) {
      return false;
    }

    // allKeyed hands the capability's own reject to `then`, so the first
    // rejection wins and nothing else has to coordinate. allSettledKeyed
    // gets a second handler over the SAME per-key record, which is what
    // makes `alreadyCalled` shared between the two.
    Local<Value> on_rejected = reject;
    if (kind == Combinator::kAllSettledKeyed) {
      Local<Function> settled_reject;
      if (!Function::New(context,
                         AllSettledKeyedRejectElement,
                         element,
                         1,
                         ConstructorBehavior::kThrow)
               .ToLocal(&settled_reject)) {
        return false;
      }
      on_rejected = settled_reject;
    }

    if (!WriteCount(context,
                    shared,
                    SlotRemaining(isolate),
                    ReadCount(context, shared, SlotRemaining(isolate)) + 1)) {
      return false;
    }

    // Invoke(nextPromise, "then", …) — a property read plus a call, so a
    // subclass that overrides `then` is honored.
    Local<Value> then_raw;
    if (!next_promise->IsObject() ||
        !next_promise.As<Object>()
             ->Get(context, FIXED_ONE_BYTE_STRING(isolate, "then"))
             .ToLocal(&then_raw)) {
      if (next_promise->IsObject()) {
        return false;
      }
      ThrowTypeError(isolate,
                     "Promise keyed combinator got a non-object from "
                     "`resolve`: it has no `then` to subscribe to");
      return false;
    }
    if (!then_raw->IsFunction()) {
      ThrowTypeError(isolate,
                     "Promise keyed combinator needs a callable `then` on "
                     "each resolved value");
      return false;
    }
    Local<Value> then_argv[] = {on_fulfilled, on_rejected};
    if (then_raw.As<Function>()
            ->Call(context, next_promise, 2, then_argv)
            .IsEmpty()) {
      return false;
    }
  }

  int32_t remaining = ReadCount(context, shared, SlotRemaining(isolate)) - 1;
  if (!WriteCount(context, shared, SlotRemaining(isolate), remaining)) {
    return false;
  }
  if (remaining == 0) {
    Local<Value> resolve_argv[] = {result};
    if (CapabilityResolve(context, capability)
            ->Call(context, v8::Undefined(isolate), 1, resolve_argv)
            .IsEmpty()) {
      return false;
    }
  }
  return true;
}

// Shared entry point: build the capability, run the body, and turn an
// abrupt body completion into a rejection of the promise we return.
static void RunKeyed(const FunctionCallbackInfo<Value>& args,
                     Combinator kind) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  Local<Object> capability;
  Local<Value> promise;
  if (!NewPromiseCapability(context, args.This(), &capability, &promise)) {
    return;
  }

  Local<Value> input = args.Length() > 0 ? args[0] : v8::Undefined(isolate);

  // IfAbruptRejectPromise: from here on a TypeError (non-object argument,
  // non-callable `resolve`, a throwing getter or proxy trap) settles the
  // returned promise instead of propagating to the caller. A caller that
  // wrote `await Promise.allKeyed(x)` sees one failure channel, not two.
  bool ok;
  {
    TryCatch try_catch(isolate);
    ok = PerformKeyed(context, args.This(), capability, input, kind);
    if (!ok) {
      if (try_catch.HasTerminated() || !try_catch.CanContinue()) {
        try_catch.ReThrow();
        return;
      }
      Local<Value> error = try_catch.Exception();
      if (error.IsEmpty()) {
        error = v8::Undefined(isolate);
      }
      try_catch.Reset();
      Local<Value> reject_argv[] = {error};
      if (CapabilityReject(context, capability)
              ->Call(context, v8::Undefined(isolate), 1, reject_argv)
              .IsEmpty()) {
        return;
      }
    }
  }

  args.GetReturnValue().Set(promise);
}

void AllKeyed(const FunctionCallbackInfo<Value>& args) {
  RunKeyed(args, Combinator::kAllKeyed);
}

void AllSettledKeyed(const FunctionCallbackInfo<Value>& args) {
  RunKeyed(args, Combinator::kAllSettledKeyed);
}

// ═══════════════════════════════════════════════════════════════════════
// Module registration.
// ═══════════════════════════════════════════════════════════════════════

// `name` and `length` are observable on a builtin, and test262 asserts both.
// `Function::New`'s length argument gives `length === 1` and `SetName` gives
// the expected `name`; V8 installs each with the standard builtin attributes
// (non-writable, non-enumerable, configurable). Constructing with
// `kThrow` also makes `new Promise.allKeyed()` a TypeError, matching a
// builtin method.
static void SetKeyedCombinator(Local<Context> context,
                               Local<Object> target,
                               Local<String> name,
                               v8::FunctionCallback callback) {
  Local<Function> fn =
      Function::New(context, callback, Local<Value>(), 1,
                    ConstructorBehavior::kThrow)
          .ToLocalChecked();
  fn->SetName(name);
  target->Set(context, name, fn).Check();
}

void Initialize(Local<Object> target,
                Local<Value> /* unused */,
                Local<Context> context,
                void* /* priv */) {
  Isolate* isolate = Isolate::GetCurrent();
  SetKeyedCombinator(
      context, target, FIXED_ONE_BYTE_STRING(isolate, "allKeyed"), AllKeyed);
  SetKeyedCombinator(context,
                     target,
                     FIXED_ONE_BYTE_STRING(isolate, "allSettledKeyed"),
                     AllSettledKeyed);
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(AllKeyed);
  registry->Register(AllSettledKeyed);
  registry->Register(CapabilityExecutor);
  registry->Register(AllKeyedElement);
  registry->Register(AllSettledKeyedFulfillElement);
  registry->Register(AllSettledKeyedRejectElement);
}

}  // namespace promise
}  // namespace socketsecurity
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(
    smol_promise,
    node::socketsecurity::promise::Initialize)
NODE_BINDING_EXTERNAL_REFERENCE(
    smol_promise,
    node::socketsecurity::promise::RegisterExternalReferences)
