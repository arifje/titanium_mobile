/*
 * Appcelerator Titanium Mobile
 * Copyright (c) 2011 by Appcelerator, Inc. All Rights Reserved.
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */

#include "ProxyFactory.h"

#include <map>
#include <stdio.h>
#include <v8.h>

#include "AndroidUtil.h"
#include "JavaObject.h"
#include "JNIUtil.h"
#include "KrollBindings.h"
#include "TypeConverter.h"
#include "V8Util.h"

#define TAG "ProxyFactory"

using namespace v8;

namespace titanium {

typedef struct {
	FunctionTemplate* v8ProxyTemplate;
	jmethodID javaProxyCreator;
} ProxyInfo;

typedef std::map<jclass, ProxyInfo> ProxyFactoryMap;
static ProxyFactoryMap factories;

#define GET_PROXY_INFO(jclass, info) \
	ProxyFactoryMap::iterator i = factories.find(jclass); \
	info = i != factories.end() ? &i->second : NULL

#define LOG_JNIENV_ERROR(msgMore) \
	LOGE(TAG, "Unable to find class %s", msgMore)

Handle<Object> ProxyFactory::createV8Proxy(jclass javaClass, jobject javaProxy)
{
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		LOG_JNIENV_ERROR("while creating Java proxy.");
		return Handle<Object>();
	}

	HandleScope scope;
	Local<Function> creator;

	ProxyInfo* info;
	GET_PROXY_INFO(javaClass, info);
	if (!info) {
		// No info has been registered for this class yet, fall back
		// to the binding lookup table
		jstring javaClassName = JNIUtil::getClassName(javaClass);
		Handle<String> className = TypeConverter::javaStringToJsString(javaClassName);
		Handle<Object> exports = KrollBindings::getBinding(className);

		if (exports.IsEmpty()) {
			String::Utf8Value classStr(className);
			LOGE(TAG, "Failed to find class for %s", *classStr);
			LOG_JNIENV_ERROR("while creating V8 Proxy.");
			return Handle<Object>();
		}

		// TODO: The first value in exports should be the type that's exported
		// But there's probably a better way to do this
		Handle<Array> names = exports->GetPropertyNames();
		if (names->Length() >= 1) {
			creator = Local<Function>::Cast(exports->Get(names->Get(0)));
		}
	} else {
		creator = info->v8ProxyTemplate->GetFunction();
	}

	Local<Value> external = External::New(javaProxy);
	Local<Object> v8Proxy = creator->NewInstance(1, &external);

	// set the pointer back on the java proxy
	jlong ptr = (jlong) *Persistent<Object>::New(v8Proxy);
	env->SetLongField(javaProxy, JNIUtil::managedV8ReferencePtrField, ptr);

	return scope.Close(v8Proxy);
}

jobject ProxyFactory::createJavaProxy(jclass javaClass, Local<Object> v8Proxy, const Arguments& args)
{
	ProxyInfo* info;
	GET_PROXY_INFO(javaClass, info);

	if (!info) {
		JNIUtil::logClassName("ProxyFactory: failed to find class for %s", javaClass, true);
		LOGE(TAG, "No proxy info found for class.");
		return NULL;
	}

	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		LOG_JNIENV_ERROR("while creating Java proxy.");
		return NULL;
	}

	// Create a persistent handle to the V8 proxy
	// and cast it to a pointer. The Java proxy needs
	// a reference to the V8 proxy for later use.
	jlong pv8Proxy = (jlong) *Persistent<Object>::New(v8Proxy);

	// We also pass the creation URL of the proxy so we can track relative URLs
	Handle<Value> sourceUrl = args.Callee()->GetScriptOrigin().ResourceName();
	String::Utf8Value sourceUrlValue(sourceUrl);

	const char *url = "app://app.js";
	if (sourceUrlValue.length() > 0) {
		url = *sourceUrlValue;
	}

	jstring javaSourceUrl = env->NewStringUTF(url);

	// Convert the V8 arguments into Java types so they can be
	// passed to the Java creator method.
	jobjectArray javaArgs = TypeConverter::jsArgumentsToJavaArray(args);

	// Create the java proxy using the creator static method provided.
	// Send along a pointer to the v8 proxy so the two are linked.
	jobject javaProxy = env->CallStaticObjectMethod(JNIUtil::krollProxyClass,
		info->javaProxyCreator, javaClass, javaArgs, pv8Proxy, javaSourceUrl);

	env->DeleteLocalRef(javaArgs);

	return javaProxy;
}

jobject ProxyFactory::unwrapJavaProxy(const Arguments& args)
{
	if (args.Length() != 1)
		return NULL;
	Local<Value> firstArgument = args[0];
	return firstArgument->IsExternal() ? (jobject)External::Unwrap(firstArgument) : NULL;
}

void ProxyFactory::registerProxyPair(jclass javaProxyClass, FunctionTemplate* v8ProxyTemplate)
{
	JNIEnv* env = JNIScope::getEnv();
	if (!env) {
		LOG_JNIENV_ERROR("while registering proxy pair.");
		return;
	}

	ProxyInfo info;
	info.v8ProxyTemplate = v8ProxyTemplate;
	info.javaProxyCreator = JNIUtil::krollProxyCreateMethod;

	factories[javaProxyClass] = info;
}

Handle<Value> ProxyFactory::proxyConstructor(const Arguments& args)
{
	HandleScope scope;
	Local<Object> v8Proxy = args.Holder();

	jclass javaClass = (jclass) External::Unwrap(args.Data());

	// If ProxyFactory::createV8Proxy invoked us, unwrap
	// the pre-created Java proxy it sent.
	jobject javaProxy = ProxyFactory::unwrapJavaProxy(args);
	bool deleteRef = false;
	if (!javaProxy) {
		javaProxy = ProxyFactory::createJavaProxy(javaClass, v8Proxy, args);
		deleteRef = true;
	}

	JavaObject* v8ProxyNative = new JavaObject(javaProxy);
	v8ProxyNative->Wrap(v8Proxy);

	if (deleteRef) {
		JNIEnv *env = JNIScope::getEnv();
		if (env) {
			env->DeleteLocalRef(javaProxy);
		}
	}

	return v8Proxy;
}

Handle<FunctionTemplate> ProxyFactory::inheritProxyTemplate(
	Persistent<FunctionTemplate> superTemplate, jclass javaClass, const char *className)
{
	HandleScope scope;

	Local<FunctionTemplate> proxyTemplate = FunctionTemplate::New(ProxyFactory::proxyConstructor, External::Wrap(javaClass));
	proxyTemplate->InstanceTemplate()->SetInternalFieldCount(ProxyFactory::kInternalFieldCount);
	proxyTemplate->SetClassName(String::NewSymbol(className));
	proxyTemplate->Inherit(superTemplate);

	registerProxyPair(javaClass, *proxyTemplate);

	return scope.Close(proxyTemplate);
}

}

