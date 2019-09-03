/*
 * Copyright (C) 2019 ProSiebenSat1.Digital GmbH.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "java-types/Deferred.h"

#include "JniCache.h"
#include "JsBridgeContext.h"
#include "StackChecker.h"
#include "jni-helpers/JniContext.h"

namespace {
  const char *PAYLOAD_PROP_NAME = "\xff\xffpayload";
  const char *PROMISE_OBJECT_PROP_NAME = "\xff\xff" "promise_object";
  const char *PROMISE_OBJECT_GLOBAL_NAME_PREFIX = "javaTypes_deferred_promiseobject_";  // Note: initial "\xff\xff" removed because of JNI string conversion issues

  struct OnPromisePayload {
    JniGlobalRef<jobject> javaDeferred;
    std::shared_ptr<const JavaType> componentType;
  };

  extern "C" {
    duk_ret_t onPromiseFulfilled(duk_context *ctx) {
      int argCount = duk_get_top(ctx);
      assert(argCount <= 1);
      CHECK_STACK_OFFSET(ctx, 0 - argCount);

      JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
      assert(jsBridgeContext != nullptr);

      JniContext *jniContext = jsBridgeContext->jniContext();

      // Get the bound native Deferred instance and the generic argument loader
      duk_push_current_function(ctx);

      if (!duk_get_prop_string(ctx, -1, PAYLOAD_PROP_NAME)) {
        duk_pop_n(ctx, 2 + argCount);  // (undefined) OnPromiseFulfilledPayload + current function + function args
        return DUK_RET_ERROR;
      }

      auto payload = reinterpret_cast<const OnPromisePayload *>(duk_get_pointer(ctx, -1));
      duk_pop_2(ctx);  // OnPromiseFulfilledPayload + current function

      // Pop promise value
      JValue value;
      if (argCount == 1) {
        value = payload->componentType->pop(true /*inScript*/);
      } else if (argCount > 1) {
        duk_pop_n(ctx, argCount);
      }

      // Complete the native Deferred
      jsBridgeContext->getJniCache()->jsBridgeInterface().resolveDeferred(payload->javaDeferred, value);

      return 0;
    }

    duk_ret_t onPromiseRejected(duk_context *ctx) {
      int argCount = duk_get_top(ctx);
      assert(argCount <= 1);
      CHECK_STACK_OFFSET(ctx, 0 - argCount);

      JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
      assert(jsBridgeContext != nullptr);

      JniContext *jniContext = jsBridgeContext->jniContext();

      // Get the bound native Deferred instance and the generic argument loader
      duk_push_current_function(ctx);

      if (!duk_get_prop_string(ctx, -1, PAYLOAD_PROP_NAME)) {
        duk_pop_n(ctx, 2 + argCount);  // (undefined) OnPromiseRejectedPayload + current function + function args
        return DUK_RET_ERROR;
      }
      auto payload = reinterpret_cast<const OnPromisePayload *>(duk_require_pointer(ctx, -1));

      duk_pop_2(ctx);  // OnPromiseRejectedPayload + current function

      JValue value;
      if (argCount == 1) {
        value = JValue(jsBridgeContext->getJavaExceptionForJsError());
      }
      duk_pop_n(ctx, argCount);  // function args

      // Reject the native Deferred
      jsBridgeContext->getJniCache()->jsBridgeInterface().rejectDeferred(payload->javaDeferred, value);
      jsBridgeContext->rethrowJniException();

      return 0;
    }

    duk_ret_t finalizeOnPromise(duk_context *ctx) {
      CHECK_STACK(ctx);

      JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
      assert(jsBridgeContext != nullptr);

      if (duk_get_prop_string(ctx, -1, PAYLOAD_PROP_NAME)) {
        delete reinterpret_cast<OnPromisePayload *>(duk_require_pointer(ctx, -1));
      }

      duk_pop(ctx);  // OnPromisePayload
      return 0;
    }

    // Add resolve and reject to bound PromiseObject instance
    duk_ret_t promiseFunction(duk_context *ctx) {
      CHECK_STACK(ctx);

      JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
      assert(jsBridgeContext != nullptr);

      duk_require_function(ctx, 0);
      duk_require_function(ctx, 1);

      duk_push_current_function(ctx);

      if (!duk_get_prop_string(ctx, -1, PROMISE_OBJECT_PROP_NAME)) {
        duk_pop_2(ctx);  // (undefined) PromiseObject + current function
        return DUK_RET_ERROR;
      }

      // Set PromiseObject.resolve and PromiseObject.reject
      duk_dup(ctx, 0);
      duk_put_prop_string(ctx, -2, "resolve");
      duk_dup(ctx, 1);
      duk_put_prop_string(ctx, -2, "reject");

      duk_pop_2(ctx);  // PromiseObject + current function
      return 0;
    }

    duk_ret_t finalizePromiseObject(duk_context *ctx) {
      CHECK_STACK(ctx);

      JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
      assert(jsBridgeContext != nullptr);

      if (duk_get_prop_string(ctx, -1, JavaTypes::Deferred::PROMISE_COMPONENT_TYPE_PROP_NAME)) {
        delete reinterpret_cast<std::shared_ptr<const JavaType> *>(duk_require_pointer(ctx, -1));
      }

      duk_pop(ctx);  // Component type ptr
      return 0;
    }
  }
}


namespace JavaTypes {

// static
const char *Deferred::PROMISE_COMPONENT_TYPE_PROP_NAME = "\xff\xff" "promise_type";

Deferred::Deferred(const JsBridgeContext *jsBridgeContext, std::unique_ptr<const JavaType> &&componentType)
 : JavaType(jsBridgeContext, JavaTypeId::Deferred)
 , m_componentType(std::move(componentType)) {
}

// JS Promise to native Deferred
JValue Deferred::pop(bool inScript) const {
  CHECK_STACK_OFFSET(m_ctx, -1);

  // Create a native Deferred instance
  JniLocalRef<jobject> javaDeferred = m_jniCache->jsBridgeInterface().createCompletableDeferred();
  if (m_jsBridgeContext->hasPendingJniException()) {
    duk_pop(m_ctx);
    m_jsBridgeContext->rethrowJniException();
  }

  if (!duk_is_object(m_ctx, -1) || !duk_has_prop_string(m_ctx, -1, "then")) {
    // Not a Promise => directly resolve the native Deferred with the value
    JValue value = m_componentType->pop(inScript);

    m_jniCache->jsBridgeInterface().resolveDeferred(javaDeferred, value);
    m_jsBridgeContext->rethrowJniException();

    return JValue(javaDeferred);
  }

  const duk_idx_t jsPromiseObjectIdx = duk_normalize_index(m_ctx, -1);
  const duk_idx_t onPromiseFulfilledIdx = duk_push_c_function(m_ctx, onPromiseFulfilled, 1);
  const duk_idx_t onPromiseRejectedIdx = duk_push_c_function(m_ctx, onPromiseRejected, 1);

  // Call JsPromise.then(onPromiseFulfilled, onPromiseRejected)
  duk_push_string(m_ctx, "then");
  duk_dup(m_ctx, onPromiseFulfilledIdx);
  duk_dup(m_ctx, onPromiseRejectedIdx);
  if (duk_pcall_prop(m_ctx, jsPromiseObjectIdx, 2) != DUK_EXEC_SUCCESS) {
    JniLocalRef<jthrowable> javaException = m_jsBridgeContext->getJavaExceptionForJsError();
    m_jniCache->jsBridgeInterface().rejectDeferred(javaDeferred, JValue(javaException));
    duk_pop_2(m_ctx);  // (undefined) ret val + JsPromiseObject
    m_jsBridgeContext->rethrowJniException();
    return JValue(javaDeferred);
  }
  duk_pop(m_ctx);  // ignored ret val

  // Bind the payload to the onPromiseFulfilled function
  auto onPromiseFulfilledPayload = new OnPromisePayload { JniGlobalRef<jobject>(javaDeferred), m_componentType };
  duk_push_pointer(m_ctx, reinterpret_cast<void *>(onPromiseFulfilledPayload));
  duk_put_prop_string(m_ctx, onPromiseFulfilledIdx, PAYLOAD_PROP_NAME);

  // Bind the payload to the onPromiseRejected function
  auto onPromiseRejectedPayload = new OnPromisePayload { JniGlobalRef<jobject>(javaDeferred), m_componentType };
  duk_push_pointer(m_ctx, reinterpret_cast<void *>(onPromiseRejectedPayload));
  duk_put_prop_string(m_ctx, onPromiseRejectedIdx, PAYLOAD_PROP_NAME);

  // Finalizer (which releases the JavaDeferred and the component Parameter global refs)
  duk_push_c_function(m_ctx, finalizeOnPromise, 1);
  duk_set_finalizer(m_ctx, onPromiseFulfilledIdx);
  duk_push_c_function(m_ctx, finalizeOnPromise, 1);
  duk_set_finalizer(m_ctx, onPromiseRejectedIdx);

  duk_pop_3(m_ctx);  // onPromiseFulfilled + onPromiseRejected + JS Promise object
  return JValue(javaDeferred);
}

// Native Deferred to JS Promise
duk_ret_t Deferred::push(const JValue &value, bool inScript) const {
  CHECK_STACK_OFFSET(m_ctx, 1);

  const JniLocalRef<jobject> &jDeferred = value.getLocalRef();

  if (jDeferred.isNull()) {
    duk_push_null(m_ctx);
    return 1;
  }

  // promiseFunction = function (resolve, reject)
  duk_push_c_function(m_ctx, promiseFunction, 2);
  // => STASH: [... promiseFunction]

  // Create a PromiseObject which will be eventually filled with {resolve, reject}
  duk_push_object(m_ctx);
  duk_push_pointer(m_ctx, new std::shared_ptr<const JavaType>(m_componentType));
  duk_put_prop_string(m_ctx, -2, PROMISE_COMPONENT_TYPE_PROP_NAME);
  // => STASH: [... promiseFunction PromiseObject]

  // Set the finalizer of the PromiseObject
  duk_push_c_function(m_ctx, finalizePromiseObject, 1);
  duk_set_finalizer(m_ctx, -2);

  static int promiseCount = 0;
  std::string promiseObjectGlobalName = PROMISE_OBJECT_GLOBAL_NAME_PREFIX + std::to_string(++promiseCount);

  // Put it to the global stash
  duk_dup_top(m_ctx);
  duk_put_global_string(m_ctx, promiseObjectGlobalName.c_str());
  // => STASH: [... promiseFunction PromiseObject]

  // Bind the PromiseObject to the promiseFunction
  duk_put_prop_string(m_ctx, -2 /*promiseFunction*/, PROMISE_OBJECT_PROP_NAME);
  // => STASH: [... promiseFunction]

  // new Promise(promiseFunction)
  if (!duk_get_global_string(m_ctx, "Promise")) {
    duk_pop_2(m_ctx);  // (undefined) "Promise" + promiseFunction
    m_jsBridgeContext->throwTypeException("Cannot push Deferred: global.Promise is undefined", inScript);
    return DUK_RET_ERROR;  // unreachable
  }
  duk_dup(m_ctx, -2 /*promiseFunction*/);
  duk_new(m_ctx, 1);  // [... "Promise" promiseFunction] => [... Promise]
  // => STASH: [... promiseFunction, Promise]

  duk_remove(m_ctx, -2);  // promiseFunction
  // => STASH: [... Promise]

  // Call Java setUpJsPromise()
  m_jniCache->jsBridgeInterface().setUpJsPromise(
    JStringLocalRef(m_jniContext, promiseObjectGlobalName.c_str()), jDeferred);
  m_jsBridgeContext->rethrowJniException();

  return 1;
}

void Deferred::completeJsPromise(const JsBridgeContext *jsBridgeContext, const std::string &strId, bool isFulfilled, const JniLocalRef<jobject> &value) {
  duk_context *ctx = jsBridgeContext->getCContext();
  assert(ctx != nullptr);

  CHECK_STACK(ctx);

  // Get the global PromiseObject
  if (!duk_get_global_string(ctx, strId.c_str())) {
    alog_warn("Could not find PromiseObject with id %s", strId.c_str());
    duk_pop(ctx);
    return;
  }

  // Get attached type ptr...
  if (!duk_get_prop_string(ctx, -1, JavaTypes::Deferred::PROMISE_COMPONENT_TYPE_PROP_NAME)) {
    alog_warn("Could not get component type from Promise with id %s", strId.c_str());
    duk_pop_2(ctx);  // (undefined) component type + PromiseObject
    return;
  }
  auto componentType = *reinterpret_cast<std::shared_ptr<const JavaType> *>(duk_require_pointer(ctx, -1));
  duk_pop(ctx);  // component type pointer

  // Get the resolve/reject function
  duk_get_prop_string(ctx, -1, isFulfilled ? "resolve" : "reject");

  // Call it with the Promise value
  componentType->push(JValue(value), false /*inScript*/);
  if (duk_pcall(ctx, 1) != DUK_EXEC_SUCCESS) {
    alog("Could not complete Promise with id %s", strId.c_str());
  }

  duk_pop_2(ctx);  // (undefined) call result + PromiseObject
}

}  // namespace JavaType

