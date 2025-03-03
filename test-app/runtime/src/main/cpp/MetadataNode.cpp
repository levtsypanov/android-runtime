#include "MetadataNode.h"
#include "NativeScriptAssert.h"
#include "Constants.h"
#include "Util.h"
#include "V8GlobalHelpers.h"
#include "ArgConverter.h"
#include "V8StringConstants.h"
#include "SimpleProfiler.h"
#include "CallbackHandlers.h"
#include "NativeScriptException.h"
#include "Runtime.h"
#include <sstream>
#include <cctype>
#include <dirent.h>
#include <errno.h>
#include <android/log.h>
#include <unistd.h>
#include "ManualInstrumentation.h"
#include "JSONObjectHelper.h"



#include "v8.h"

using namespace v8;
using namespace std;
using namespace tns;

void MetadataNode::Init(Isolate* isolate) {
    auto key = ArgConverter::ConvertToV8String(isolate, "tns::MetadataKey");
    auto cache = GetMetadataNodeCache(isolate);
    cache->MetadataKey = new Persistent<String>(isolate, key);
}

Local<ObjectTemplate> MetadataNode::GetOrCreateArrayObjectTemplate(Isolate* isolate) {
    auto it = s_arrayObjectTemplates.find(isolate);
    if (it != s_arrayObjectTemplates.end()) {
        return it->second->Get(isolate);
    }

    auto arrayObjectTemplate = ObjectTemplate::New(isolate);
    arrayObjectTemplate->SetInternalFieldCount(static_cast<int>(ObjectManager::MetadataNodeKeys::END));
    arrayObjectTemplate->SetIndexedPropertyHandler(ArrayIndexedPropertyGetterCallback, ArrayIndexedPropertySetterCallback);

    s_arrayObjectTemplates.emplace(std::make_pair(isolate, new Persistent<ObjectTemplate>(isolate, arrayObjectTemplate)));

    return arrayObjectTemplate;
}

MetadataNode::MetadataNode(MetadataTreeNode* treeNode) :
    m_treeNode(treeNode) {
    uint8_t nodeType = s_metadataReader.GetNodeType(treeNode);

    m_name = s_metadataReader.ReadTypeName(m_treeNode);

    uint8_t parentNodeType = s_metadataReader.GetNodeType(treeNode->parent);

    m_isArray = s_metadataReader.IsNodeTypeArray(parentNodeType);

    bool isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);

    if (!m_isArray && isInterface) {
        bool isPrefix;
        auto impTypeName = s_metadataReader.ReadInterfaceImplementationTypeName(m_treeNode, isPrefix);
        m_implType = isPrefix
                     ? (impTypeName + m_name)
                     :
                     impTypeName;
    }
}

Local<Object> MetadataNode::CreateExtendedJSWrapper(Isolate* isolate, ObjectManager* objectManager, const string& proxyClassName) {
    Local<Object> extInstance;

    auto cacheData = GetCachedExtendedClassData(isolate, proxyClassName);
    if (cacheData.node != nullptr) {
        extInstance = objectManager->GetEmptyObject(isolate);
        extInstance->SetInternalField(static_cast<int>(ObjectManager::MetadataNodeKeys::CallSuper), True(isolate));
        auto extdCtorFunc = Local<Function>::New(isolate, *cacheData.extendedCtorFunction);
        auto context = Runtime::GetRuntime(isolate)->GetContext();
        extInstance->SetPrototype(context, extdCtorFunc->Get(context, V8StringConstants::GetPrototype(isolate)).ToLocalChecked());
        extInstance->Set(context, ArgConverter::ConvertToV8String(isolate, "constructor"), extdCtorFunc);

        SetInstanceMetadata(isolate, extInstance, cacheData.node);
    }

    return extInstance;
}

MetadataNode* MetadataNode::GetOrCreate(const string& className) {
    MetadataNode* node = nullptr;

    auto it = s_name2NodeCache.find(className);

    if (it == s_name2NodeCache.end()) {
        MetadataTreeNode* treeNode = GetOrCreateTreeNodeByName(className);

        node = GetOrCreateInternal(treeNode);

        s_name2NodeCache.insert(make_pair(className, node));
    } else {
        node = it->second;
    }

    return node;
}

MetadataNode* MetadataNode::GetOrCreateInternal(MetadataTreeNode* treeNode) {
    MetadataNode* result = nullptr;

    auto it = s_treeNode2NodeCache.find(treeNode);

    if (it != s_treeNode2NodeCache.end()) {
        result = it->second;
    } else {
        result = new MetadataNode(treeNode);

        s_treeNode2NodeCache.insert(make_pair(treeNode, result));
    }

    return result;
}

MetadataTreeNode* MetadataNode::GetOrCreateTreeNodeByName(const string& className) {
    MetadataTreeNode* result = nullptr;

    auto itFound = s_name2TreeNodeCache.find(className);

    if (itFound != s_name2TreeNodeCache.end()) {
        result = itFound->second;
    } else {
        result = s_metadataReader.GetOrCreateTreeNodeByName(className);

        s_name2TreeNodeCache.insert(make_pair(className, result));
    }

    return result;
}

string MetadataNode::GetName() {
    return m_name;
}

bool MetadataNode::IsNodeTypeInterface() {
    uint8_t nodeType = s_metadataReader.GetNodeType(m_treeNode);
    return s_metadataReader.IsNodeTypeInterface(nodeType);
}

string MetadataNode::GetTypeMetadataName(Isolate* isolate, Local<Value>& value) {
    auto data = GetTypeMetadata(isolate, value.As<Function>());

    return data->name;
}

Local<Object> MetadataNode::CreateWrapper(Isolate* isolate) {
    EscapableHandleScope handle_scope(isolate);

    Local<Object> obj;

    uint8_t nodeType = s_metadataReader.GetNodeType(m_treeNode);

    bool isClass = s_metadataReader.IsNodeTypeClass(nodeType),
         isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);

    if (isClass || isInterface) {
        obj = GetConstructorFunction(isolate);
    } else if (s_metadataReader.IsNodeTypePackage(nodeType)) {
        obj = CreatePackageObject(isolate);
    } else {
        stringstream ss;
        ss << "(InternalError): Can't create proxy for this type=" << static_cast<int>(nodeType);
        throw NativeScriptException(ss.str());
    }

    return handle_scope.Escape(obj);
}

Local<Object> MetadataNode::CreateJSWrapper(Isolate* isolate, ObjectManager* objectManager) {
    Local<Object> obj;

    if (m_isArray) {
        obj = CreateArrayWrapper(isolate);
    } else {
        obj = objectManager->GetEmptyObject(isolate);
        if (!obj.IsEmpty()) {
            auto ctorFunc = GetConstructorFunction(isolate);
            auto context = isolate->GetCurrentContext();
            obj->Set(context, ArgConverter::ConvertToV8String(isolate, "constructor"), ctorFunc);
            obj->SetPrototype(context, ctorFunc->Get(context, V8StringConstants::GetPrototype(isolate)).ToLocalChecked());
            SetInstanceMetadata(isolate, obj, this);
        }
    }

    return obj;
}

void MetadataNode::ArrayLengthGetterCallack(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto isolate = info.GetIsolate();
        auto length = CallbackHandlers::GetArrayLength(isolate, thiz);
        info.GetReturnValue().Set(Integer::New(isolate, length));
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

Local<Object> MetadataNode::CreateArrayWrapper(Isolate* isolate) {
    auto node = GetOrCreate("java/lang/Object");
    auto ctorFunc = node->GetConstructorFunction(isolate);

    auto arrayObjectTemplate = GetOrCreateArrayObjectTemplate(isolate);

    auto context = isolate->GetCurrentContext();
    auto arr = arrayObjectTemplate->NewInstance(context).ToLocalChecked();
    arr->SetPrototype(context, ctorFunc->Get(context, V8StringConstants::GetPrototype(isolate)).ToLocalChecked());
    arr->SetAccessor(context, ArgConverter::ConvertToV8String(isolate, "length"), ArrayLengthGetterCallack, nullptr, Local<Value>(), AccessControl::ALL_CAN_READ, PropertyAttribute::DontDelete);

    SetInstanceMetadata(isolate, arr, this);

    return arr;
}

Local<Object> MetadataNode::CreatePackageObject(Isolate* isolate) {
    auto packageObj = Object::New(isolate);
    auto ptrChildren = this->m_treeNode->children;
    if (ptrChildren != nullptr) {
        auto ctx = isolate->GetCurrentContext();
        auto extData = External::New(isolate, this);
        const auto& children = *ptrChildren;
        for (auto childNode: children) {
            packageObj->SetAccessor(ctx, ArgConverter::ConvertToV8String(isolate, childNode->name),
                                    PackageGetterCallback,
                                    nullptr,
                                    extData);
        }
    }

    return packageObj;
}

void MetadataNode::SetClassAccessor(Local<Function>& ctorFunction) {
    auto isolate = ctorFunction->GetIsolate();
    auto classFieldName = ArgConverter::ConvertToV8String(isolate, "class");
    auto context = isolate->GetCurrentContext();
    ctorFunction->SetAccessor(context, classFieldName, ClassAccessorGetterCallback, nullptr, Local<Value>(), AccessControl::ALL_CAN_READ, PropertyAttribute::DontDelete);
}

void MetadataNode::ClassAccessorGetterCallback(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto isolate = info.GetIsolate();
        auto data = GetTypeMetadata(isolate, thiz.As<Function>());

        auto value = CallbackHandlers::FindClass(isolate, data->name);
        info.GetReturnValue().Set(value);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::NullObjectAccessorGetterCallback(Local<Name> property,const PropertyCallbackInfo<Value>& info) {
    try {
        DEBUG_WRITE("NullObjectAccessorGetterCallback called");
        auto isolate = info.GetIsolate();

        auto thiz = info.This();
        Local<Value> hiddenVal;
        V8GetPrivateValue(isolate, thiz, V8StringConstants::GetNullNodeName(isolate), hiddenVal);
        if (hiddenVal.IsEmpty()) {
            auto node = reinterpret_cast<MetadataNode*>(info.Data().As<External>()->Value());
            V8SetPrivateValue(isolate, thiz, V8StringConstants::GetNullNodeName(isolate), External::New(isolate, node));
            auto funcTemplate = FunctionTemplate::New(isolate, MetadataNode::NullValueOfCallback);
            auto context = isolate->GetCurrentContext();
            thiz->Delete(context, V8StringConstants::GetValueOf(isolate));
            thiz->Set(context, V8StringConstants::GetValueOf(isolate), funcTemplate->GetFunction(context).ToLocalChecked());
        }

        info.GetReturnValue().Set(thiz);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::NullValueOfCallback(const FunctionCallbackInfo<Value>& args) {
    try {
        auto isolate = args.GetIsolate();
        args.GetReturnValue().Set(Null(isolate));
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::FieldAccessorGetterCallback(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto fieldCallbackData = reinterpret_cast<FieldCallbackData*>(info.Data().As<External>()->Value());

        if ((!fieldCallbackData->isStatic && thiz->StrictEquals(info.Holder()))
                // check whether there's a declaring type to get the class from it
                || (fieldCallbackData->declaringType == "")) {
            info.GetReturnValue().SetUndefined();
            return;
        }

        auto isolate = info.GetIsolate();
        auto value = CallbackHandlers::GetJavaField(isolate, thiz, fieldCallbackData);
        info.GetReturnValue().Set(value);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}
void MetadataNode::FieldAccessorSetterCallback(Local<Name> property, Local<Value> value, const PropertyCallbackInfo<void>& info) {
    try {
        auto thiz = info.This();
        auto fieldCallbackData = reinterpret_cast<FieldCallbackData*>(info.Data().As<External>()->Value());

        if (!fieldCallbackData->isStatic && thiz->StrictEquals(info.Holder())) {
            auto isolate = info.GetIsolate();
            info.GetReturnValue().Set(v8::Undefined(isolate));
            return;
        }

        if (fieldCallbackData->isFinal) {
            stringstream ss;
            ss << "You are trying to set \"" << fieldCallbackData->name << "\" which is a final field! Final fields can only be read.";
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        } else {
            auto isolate = info.GetIsolate();
            CallbackHandlers::SetJavaField(isolate, thiz, value, fieldCallbackData);
            info.GetReturnValue().Set(value);
        }
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::PropertyAccessorGetterCallback(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        auto isolate = info.GetIsolate();
        auto context = isolate->GetCurrentContext();
        auto thiz = info.This();
        auto propertyCallbackData = reinterpret_cast<PropertyCallbackData*>(info.Data().As<External>()->Value());

        std::string getterMethodName = propertyCallbackData->getterMethodName;
        if(getterMethodName == ""){
            throw NativeScriptException("Missing getter method for property: " + propertyCallbackData->propertyName);
        }

        auto getter = thiz->Get(context, v8::String::NewFromUtf8(isolate, getterMethodName.c_str()).ToLocalChecked()).ToLocalChecked();
        auto value = getter.As<Function>()->Call(context, thiz, 0, nullptr).ToLocalChecked();
        info.GetReturnValue().Set(value);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}
void MetadataNode::PropertyAccessorSetterCallback(Local<Name> property, Local<Value> value, const PropertyCallbackInfo<void>& info) {
    try {
        auto isolate = info.GetIsolate();
        auto context = isolate->GetCurrentContext();
        auto thiz = info.This();
        auto propertyCallbackData = reinterpret_cast<PropertyCallbackData*>(info.Data().As<External>()->Value());

        std::string setterMethodName = propertyCallbackData->setterMethodName;
        if(setterMethodName == ""){
            throw NativeScriptException("Missing setter method for property: " + propertyCallbackData->propertyName);
        }

        auto setter = thiz->Get(context, v8::String::NewFromUtf8(isolate, setterMethodName.c_str()).ToLocalChecked()).ToLocalChecked();
        setter.As<Function>()->Call(context, thiz, 1, &value).ToLocalChecked();
        info.GetReturnValue().Set(value);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::SuperAccessorGetterCallback(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto isolate = info.GetIsolate();
        auto key = ArgConverter::ConvertToV8String(isolate, "supervalue");
        Local<Value> hidenVal;
        V8GetPrivateValue(isolate, thiz, key, hidenVal);
        auto superValue = hidenVal.As<Object>();

        if (superValue.IsEmpty()) {
            auto runtime = Runtime::GetRuntime(isolate);
            auto objectManager = runtime->GetObjectManager();
            auto context = isolate->GetCurrentContext();

            superValue = objectManager->GetEmptyObject(isolate);
            superValue->Delete(context, V8StringConstants::GetToString(isolate));
            superValue->Delete(context, V8StringConstants::GetValueOf(isolate));
            superValue->SetInternalField(static_cast<int>(ObjectManager::MetadataNodeKeys::CallSuper), True(isolate));

            superValue->SetPrototype(context, thiz->GetPrototype().As<Object>()->GetPrototype().As<Object>()->GetPrototype());
            V8SetPrivateValue(isolate, thiz, key, superValue);
            objectManager->CloneLink(thiz, superValue);

            DEBUG_WRITE("superValue.GetPrototype=%d", superValue->GetPrototype().As<Object>()->GetIdentityHash());

            auto node = GetInstanceMetadata(isolate, thiz);
            SetInstanceMetadata(isolate, superValue, node);
        }

        info.GetReturnValue().Set(superValue);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

std::vector<MetadataNode::MethodCallbackData*> MetadataNode::SetInstanceMembers(Isolate* isolate, Local<FunctionTemplate>& ctorFuncTemplate, Local<ObjectTemplate>& prototypeTemplate, vector<MethodCallbackData*>& instanceMethodsCallbackData, const vector<MethodCallbackData*>& baseInstanceMethodsCallbackData, MetadataTreeNode* treeNode) {
    auto hasCustomMetadata = treeNode->metadata != nullptr;

    if (hasCustomMetadata) {
        return SetInstanceMembersFromRuntimeMetadata(isolate, ctorFuncTemplate, prototypeTemplate, instanceMethodsCallbackData, baseInstanceMethodsCallbackData, treeNode);
    } else {
        SetInstanceFieldsFromStaticMetadata(isolate, ctorFuncTemplate, prototypeTemplate, instanceMethodsCallbackData, baseInstanceMethodsCallbackData, treeNode);
        return SetInstanceMethodsFromStaticMetadata(isolate, ctorFuncTemplate, prototypeTemplate, instanceMethodsCallbackData, baseInstanceMethodsCallbackData, treeNode);
    }
}

vector<MetadataNode::MethodCallbackData *> MetadataNode::SetInstanceMethodsFromStaticMetadata(Isolate *isolate,
                                                   Local<FunctionTemplate> &ctorFuncTemplate,
                                                   Local<ObjectTemplate> &prototypeTemplate,
                                                   vector<MethodCallbackData *> &instanceMethodsCallbackData,
                                                   const vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
                                                   MetadataTreeNode *treeNode) {
    SET_PROFILER_FRAME();

    std::vector<MethodCallbackData *> instanceMethodData;

    uint8_t *curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;

    auto nodeType = s_metadataReader.GetNodeType(treeNode);

    auto curType = s_metadataReader.ReadTypeName(treeNode);

    curPtr += sizeof(uint16_t /* baseClassId */);

    if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
        curPtr += sizeof(uint8_t) + sizeof(uint32_t);
    }

    string lastMethodName;
    MethodCallbackData *callbackData = nullptr;

    auto context = isolate->GetCurrentContext();
    auto origin = Constants::APP_ROOT_FOLDER_PATH + GetOrCreateInternal(treeNode)->m_name;

    std::unordered_map<std::string, MethodCallbackData *> collectedExtensionMethodDatas;

    auto extensionFunctionsCount = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < extensionFunctionsCount; i++) {
        auto entry = s_metadataReader.ReadExtensionFunctionEntry(&curPtr);

        if (entry.name != lastMethodName) {
            //
            callbackData = tryGetExtensionMethodCallbackData(collectedExtensionMethodDatas,
                                                             entry.name);
            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);
                auto funcData = External::New(isolate, callbackData);
                auto funcTemplate = FunctionTemplate::New(isolate, MethodCallback, funcData);
                auto funcName = ArgConverter::ConvertToV8String(isolate, entry.name);
                prototypeTemplate->Set(funcName, funcTemplate);

                lastMethodName = entry.name;
                std::pair<std::string, MethodCallbackData *> p(entry.name, callbackData);
                collectedExtensionMethodDatas.insert(p);
            }
        }
        callbackData->candidates.push_back(entry);
    }


    //get candidates from instance methods metadata
    auto instanceMethodCount = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);

    for (auto i = 0; i < instanceMethodCount; i++) {
        auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);

        // attach a function to the prototype of a javascript Object
        if (entry.name != lastMethodName) {
            // See if we have tracked the callback data before (meaning another version of entry.name exists with different parameters)
            callbackData = tryGetExtensionMethodCallbackData(collectedExtensionMethodDatas,
                                                             entry.name);
            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);

                // If we have no tracking of this callback data, create tracking so that we can find it if need be for future itterations where the entry.name is the same...
                std::pair<std::string, MethodCallbackData *> p(entry.name, callbackData);
                collectedExtensionMethodDatas.insert(p);
            }

            instanceMethodData.push_back(callbackData);

            instanceMethodsCallbackData.push_back(callbackData);
            auto itBegin = baseInstanceMethodsCallbackData.begin();
            auto itEnd = baseInstanceMethodsCallbackData.end();
            auto itFound = find_if(itBegin, itEnd, [&entry](MethodCallbackData *x) {
                return x->candidates.front().name == entry.name;
            });
            if (itFound != itEnd) {
                callbackData->parent = *itFound;
            }

            auto funcData = External::New(isolate, callbackData);
            auto funcTemplate = FunctionTemplate::New(isolate, MethodCallback, funcData);

            auto funcName = ArgConverter::ConvertToV8String(isolate, entry.name);

            if (s_profilerEnabled) {
                auto func = funcTemplate->GetFunction(context).ToLocalChecked();
                Local<Function> wrapperFunc = Wrap(isolate, func, entry.name, origin,
                                                   false /* isCtorFunc */);
                Local<Function> ctorFunc = ctorFuncTemplate->GetFunction(context).ToLocalChecked();
                Local<Value> protoVal;
                ctorFunc->Get(context,
                              ArgConverter::ConvertToV8String(isolate, "prototype")).ToLocal(
                        &protoVal);
                if (!protoVal.IsEmpty() && !protoVal->IsUndefined() && !protoVal->IsNull()) {
                    protoVal.As<Object>()->Set(context, funcName, wrapperFunc);
                }
            } else {
                prototypeTemplate->Set(funcName, funcTemplate);
            }

            lastMethodName = entry.name;
        }

        callbackData->candidates.push_back(entry);
    }

    return instanceMethodData;
}

MetadataNode::MethodCallbackData *MetadataNode::tryGetExtensionMethodCallbackData(
        std::unordered_map<std::string, MethodCallbackData *> collectedMethodCallbackDatas,
        std::string lookupName) {

    if (collectedMethodCallbackDatas.size() < 1) {
        return nullptr;
    }

    auto iter = collectedMethodCallbackDatas.find(lookupName);

    if (iter != collectedMethodCallbackDatas.end()) {
        return iter->second;
    }

    return nullptr;
}

void MetadataNode::SetInstanceFieldsFromStaticMetadata(Isolate* isolate, Local<FunctionTemplate>& ctorFuncTemplate, Local<ObjectTemplate>& prototypeTemplate, vector<MethodCallbackData*>& instanceMethodsCallbackData, const vector<MethodCallbackData*>& baseInstanceMethodsCallbackData, MetadataTreeNode* treeNode) {
    SET_PROFILER_FRAME();

    Local<Function> ctorFunction;

    uint8_t* curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;

    auto nodeType = s_metadataReader.GetNodeType(treeNode);

    auto curType = s_metadataReader.ReadTypeName(treeNode);

    curPtr += sizeof(uint16_t /* baseClassId */);

    if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
        curPtr += sizeof(uint8_t) + sizeof(uint32_t);
    }

    auto extensionFunctionsCount = *reinterpret_cast<uint16_t*>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < extensionFunctionsCount; i++) {
        auto entry = s_metadataReader.ReadExtensionFunctionEntry(&curPtr);
    }

    //get candidates from instance methods metadata
    auto instanceMethodCout = *reinterpret_cast<uint16_t*>(curPtr);
    curPtr += sizeof(uint16_t);

    //skip metadata methods -- advance the pointer only
    for (auto i = 0; i < instanceMethodCout; i++) {
        auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
    }

    //get candidates from instance fields metadata
    auto instanceFieldCout = *reinterpret_cast<uint16_t*>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < instanceFieldCout; i++) {
        auto entry = s_metadataReader.ReadInstanceFieldEntry(&curPtr);

        auto fieldName = ArgConverter::ConvertToV8String(isolate, entry.name);
        auto fieldInfo = new FieldCallbackData(entry);
        fieldInfo->declaringType = curType;
        auto fieldData = External::New(isolate, fieldInfo);
        prototypeTemplate->SetAccessor(fieldName, FieldAccessorGetterCallback, FieldAccessorSetterCallback, fieldData, AccessControl::DEFAULT, PropertyAttribute::DontDelete);
    }

    auto kotlinPropertiesCount = *reinterpret_cast<uint16_t*>(curPtr);
    curPtr += sizeof(uint16_t);
    for (int i = 0; i < kotlinPropertiesCount; ++i) {
        uint32_t nameOfffset = *reinterpret_cast<uint32_t*>(curPtr);
        string propertyName = s_metadataReader.ReadName(nameOfffset);
        curPtr += sizeof(uint32_t);

        auto hasGetter = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);

        std::string getterMethodName = "";
        if(hasGetter>=1){
            auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
            getterMethodName = entry.name;
        }

        auto hasSetter = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);

        std::string setterMethodName = "";
        if(hasSetter >= 1){
            auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
            setterMethodName = entry.name;
        }

        auto propertyInfo = new PropertyCallbackData(propertyName, getterMethodName, setterMethodName);
        auto propertyData = External::New(isolate, propertyInfo);
        prototypeTemplate->SetAccessor(ArgConverter::ConvertToV8String(isolate, propertyName), PropertyAccessorGetterCallback, PropertyAccessorSetterCallback, propertyData, AccessControl::DEFAULT, PropertyAttribute::DontDelete);
    }
}

vector<MetadataNode::MethodCallbackData*> MetadataNode::SetInstanceMembersFromRuntimeMetadata(Isolate* isolate, Local<FunctionTemplate>& ctorFuncTemplate, Local<ObjectTemplate>& prototypeTemplate, vector<MethodCallbackData*>& instanceMethodsCallbackData, const vector<MethodCallbackData*>& baseInstanceMethodsCallbackData, MetadataTreeNode* treeNode) {
    SET_PROFILER_FRAME();

    assert(treeNode->metadata != nullptr);

    std::vector<MethodCallbackData*> instanceMethodData;

    string line;
    const string& metadata = *treeNode->metadata;
    stringstream s(metadata);

    string kind;
    string name;
    string signature;
    int paramCount;

    getline(s, line); // type line
    getline(s, line); // base class line

    string lastMethodName;
    MethodCallbackData* callbackData = nullptr;

    while (getline(s, line)) {
        stringstream tmp(line);
        tmp >> kind >> name >> signature >> paramCount;

        char chKind = kind[0];

        // method or field
        assert((chKind == 'M') || (chKind == 'F'));

        MetadataEntry entry;
        entry.name = name;
        entry.sig = signature;
        MetadataReader::FillReturnType(entry);
        entry.paramCount = paramCount;
        entry.isStatic = false;

        if (chKind == 'M') {
            if (entry.name != lastMethodName) {
                callbackData = new MethodCallbackData(this);
                instanceMethodData.push_back(callbackData);

                instanceMethodsCallbackData.push_back(callbackData);
                auto itBegin = baseInstanceMethodsCallbackData.begin();
                auto itEnd = baseInstanceMethodsCallbackData.end();
                auto itFound = find_if(itBegin, itEnd, [&entry] (MethodCallbackData *x) {
                    return x->candidates.front().name == entry.name;
                });
                if (itFound != itEnd) {
                    callbackData->parent = *itFound;
                }

                auto funcData = External::New(isolate, callbackData);
                auto funcTemplate = FunctionTemplate::New(isolate, MethodCallback, funcData);
                auto funcName = ArgConverter::ConvertToV8String(isolate, entry.name);
                prototypeTemplate->Set(funcName, funcTemplate);
                lastMethodName = entry.name;
            }
            callbackData->candidates.push_back(entry);
        } else if (chKind == 'F') {
            auto fieldName = ArgConverter::ConvertToV8String(isolate, entry.name);
            auto fieldData = External::New(isolate, new FieldCallbackData(entry));
            auto access = entry.isFinal ? AccessControl::ALL_CAN_READ : AccessControl::DEFAULT;
            prototypeTemplate->SetAccessor(fieldName, FieldAccessorGetterCallback, FieldAccessorSetterCallback, fieldData, access, PropertyAttribute::DontDelete);
        }
    }
    return instanceMethodData;
}

void MetadataNode::SetStaticMembers(Isolate* isolate, Local<Function>& ctorFunction, MetadataTreeNode* treeNode) {
    auto hasCustomMetadata = treeNode->metadata != nullptr;
    auto context = isolate->GetCurrentContext();

    if (!hasCustomMetadata) {
        uint8_t* curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;
        auto nodeType = s_metadataReader.GetNodeType(treeNode);
        auto curType = s_metadataReader.ReadTypeName(treeNode);
        curPtr += sizeof(uint16_t /* baseClassId */);
        if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
            curPtr += sizeof(uint8_t) + sizeof(uint32_t);
        }

        auto extensionFunctionsCount = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (auto i = 0; i < extensionFunctionsCount; i++) {
            auto entry = s_metadataReader.ReadExtensionFunctionEntry(&curPtr);
        }

        auto instanceMethodCout = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (auto i = 0; i < instanceMethodCout; i++) {
            auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
        }

        auto instanceFieldCout = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (auto i = 0; i < instanceFieldCout; i++) {
            auto entry = s_metadataReader.ReadInstanceFieldEntry(&curPtr);
        }

        auto kotlinPropertiesCount = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (int i = 0; i < kotlinPropertiesCount; ++i) {
            uint32_t nameOfffset = *reinterpret_cast<uint32_t*>(curPtr);
            string propertyName = s_metadataReader.ReadName(nameOfffset);
            curPtr += sizeof(uint32_t);

            auto hasGetter = *reinterpret_cast<uint16_t*>(curPtr);
            curPtr += sizeof(uint16_t);

            if(hasGetter>=1){
                auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
            }

            auto hasSetter = *reinterpret_cast<uint16_t*>(curPtr);
            curPtr += sizeof(uint16_t);

            if(hasSetter >= 1){
                auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);
            }
        }

        string lastMethodName;
        MethodCallbackData* callbackData = nullptr;

        auto origin = Constants::APP_ROOT_FOLDER_PATH + GetOrCreateInternal(treeNode)->m_name;

        //get candidates from static methods metadata
        auto staticMethodCout = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (auto i = 0; i < staticMethodCout; i++) {
            auto entry = s_metadataReader.ReadStaticMethodEntry(&curPtr);
            if (entry.name != lastMethodName) {
                callbackData = new MethodCallbackData(this);
                auto funcData = External::New(isolate, callbackData);
                auto funcTemplate = FunctionTemplate::New(isolate, MethodCallback, funcData);
                auto func = funcTemplate->GetFunction(context).ToLocalChecked();
                auto funcName = ArgConverter::ConvertToV8String(isolate, entry.name);
                ctorFunction->Set(context, funcName, Wrap(isolate, func, entry.name, origin, false /* isCtorFunc */));
                lastMethodName = entry.name;
            }
            callbackData->candidates.push_back(entry);
        }

        //attach .extend function
        auto extendFuncName = V8StringConstants::GetExtend(isolate);
        auto extendFuncTemplate = FunctionTemplate::New(isolate, ExtendMethodCallback, External::New(isolate, this));
        ctorFunction->Set(context, extendFuncName, extendFuncTemplate->GetFunction(context).ToLocalChecked());

        //get candidates from static fields metadata
        auto staticFieldCout = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        for (auto i = 0; i < staticFieldCout; i++) {
            auto entry = s_metadataReader.ReadStaticFieldEntry(&curPtr);

            auto fieldName = ArgConverter::ConvertToV8String(isolate, entry.name);
            auto fieldData = External::New(isolate, new FieldCallbackData(entry));
            ctorFunction->SetAccessor(context, fieldName, FieldAccessorGetterCallback, FieldAccessorSetterCallback, fieldData, AccessControl::DEFAULT, PropertyAttribute::DontDelete);
        }

        auto nullObjectName = V8StringConstants::GetNullObject(isolate);

        Local<Value> nullObjectData = External::New(isolate, this);
        ctorFunction->SetAccessor(context, nullObjectName, NullObjectAccessorGetterCallback, nullptr, nullObjectData);

        SetClassAccessor(ctorFunction);
    }
}

void MetadataNode::SetInnerTypes(Isolate* isolate, Local<Function>& ctorFunction, MetadataTreeNode* treeNode) {
    auto context = isolate->GetCurrentContext();
    if (treeNode->children != nullptr) {
        const auto& children = *treeNode->children;

        for (auto curChild : children) {
            auto childNode = GetOrCreateInternal(curChild);

            // The call to GetConstructorFunctionTemplate bootstraps the ctor function for the childNode
            auto innerTypeCtorFuncTemplate = childNode->GetConstructorFunctionTemplate(isolate, curChild);
            auto innerTypeCtorFunc = Local<Function>::New(isolate, *GetOrCreateInternal(curChild)->GetPersistentConstructorFunction(isolate));
            auto innerTypeName = ArgConverter::ConvertToV8String(isolate, curChild->name);
            ctorFunction->Set(context, innerTypeName, innerTypeCtorFunc);
        }
    }
}

Local<FunctionTemplate> MetadataNode::GetConstructorFunctionTemplate(Isolate* isolate, MetadataTreeNode* treeNode) {
    std::vector<MethodCallbackData*> instanceMethodsCallbackData;

    v8::HandleScope handleScope(isolate);
    auto ft = GetConstructorFunctionTemplate(isolate, treeNode, instanceMethodsCallbackData);

    return ft;
}

Local<FunctionTemplate> MetadataNode::GetConstructorFunctionTemplate(Isolate* isolate, MetadataTreeNode* treeNode, vector<MethodCallbackData*>& instanceMethodsCallbackData) {
    SET_PROFILER_FRAME();
    tns::instrumentation::Frame frame;

    //try get cached "ctorFuncTemplate"
    Local<FunctionTemplate> ctorFuncTemplate;
    auto cache = GetMetadataNodeCache(isolate);
    auto itFound = cache->CtorFuncCache.find(treeNode);
    if (itFound != cache->CtorFuncCache.end()) {
        auto& ctorCacheItem = itFound->second;
        instanceMethodsCallbackData = ctorCacheItem.instanceMethodCallbacks;
        ctorFuncTemplate = Local<FunctionTemplate>::New(isolate, *ctorCacheItem.ft);
        return ctorFuncTemplate;
    }
    //

    auto node = GetOrCreateInternal(treeNode);
    auto ctorCallbackData = External::New(isolate, node);
    auto isInterface = s_metadataReader.IsNodeTypeInterface(treeNode->type);
    auto funcCallback = isInterface ? InterfaceConstructorCallback : ClassConstructorCallback;
    ctorFuncTemplate = FunctionTemplate::New(isolate, funcCallback, ctorCallbackData);
    ctorFuncTemplate->InstanceTemplate()->SetInternalFieldCount(static_cast<int>(ObjectManager::MetadataNodeKeys::END));

    Local<Function> baseCtorFunc;
    std::vector<MethodCallbackData*> baseInstanceMethodsCallbackData;
    auto tmpTreeNode = treeNode;
    JEnv env;
    auto currentClass = env.FindClass(node->m_name);
    std::vector<MetadataTreeNode*> skippedBaseTypes;
    while (true) {
        auto baseTreeNode = s_metadataReader.GetBaseClassNode(tmpTreeNode);
        if (CheckClassHierarchy(env, currentClass, treeNode, baseTreeNode, skippedBaseTypes)) {
            tmpTreeNode = baseTreeNode;
            continue;
        }
        if ((baseTreeNode != treeNode) && (baseTreeNode != nullptr) && (baseTreeNode->offsetValue > 0)) {
            auto baseFuncTemplate = GetConstructorFunctionTemplate(isolate, baseTreeNode, baseInstanceMethodsCallbackData);
            if (!baseFuncTemplate.IsEmpty()) {
                ctorFuncTemplate->Inherit(baseFuncTemplate);
                baseCtorFunc = Local<Function>::New(isolate, *GetOrCreateInternal(baseTreeNode)->GetPersistentConstructorFunction(isolate));
            }
        }
        break;
    }

    auto prototypeTemplate = ctorFuncTemplate->PrototypeTemplate();

    auto instanceMethodData = node->SetInstanceMembers(isolate, ctorFuncTemplate, prototypeTemplate, instanceMethodsCallbackData, baseInstanceMethodsCallbackData, treeNode);
    if (!skippedBaseTypes.empty()) {
        node->SetMissingBaseMethods(isolate, skippedBaseTypes, instanceMethodData, prototypeTemplate);
    }

    auto context = isolate->GetCurrentContext();
    auto ctorFunc = ctorFuncTemplate->GetFunction(context).ToLocalChecked();

    auto origin = Constants::APP_ROOT_FOLDER_PATH + node->m_name;

    auto wrappedCtorFunc = Wrap(isolate, ctorFunc, node->m_treeNode->name, origin, true /* isCtorFunc */);

    node->SetStaticMembers(isolate, wrappedCtorFunc, treeNode);

    // insert isolate-specific persistent function handle
    node->m_poCtorCachePerIsolate.insert({isolate, new Persistent<Function>(isolate, wrappedCtorFunc)});
    if (!baseCtorFunc.IsEmpty()) {
        auto context = isolate->GetCurrentContext();
        wrappedCtorFunc->SetPrototype(context, baseCtorFunc);
    }

    //cache "ctorFuncTemplate"
    auto pft = new Persistent<FunctionTemplate>(isolate, ctorFuncTemplate);
    CtorCacheData ctorCacheItem(pft, instanceMethodsCallbackData);
    cache->CtorFuncCache.insert(make_pair(treeNode, ctorCacheItem));

    SetInnerTypes(isolate, wrappedCtorFunc, treeNode);

    SetTypeMetadata(isolate, wrappedCtorFunc, new TypeMetadata(s_metadataReader.ReadTypeName(treeNode)));

    if (frame.check()) {
        frame.log("Materializing class: " + node->m_name);
    }

    return ctorFuncTemplate;
}

Local<Function> MetadataNode::GetConstructorFunction(Isolate* isolate) {
    GetConstructorFunctionTemplate(isolate, m_treeNode);
    auto ctorFunc = Local<Function>::New(isolate, *GetPersistentConstructorFunction(isolate));

    return ctorFunc;
}

Persistent<Function>* MetadataNode::GetPersistentConstructorFunction(Isolate* isolate) {
    auto itFound = m_poCtorCachePerIsolate.find(isolate);
    if (itFound != m_poCtorCachePerIsolate.end()) {
        auto& constrFunction = itFound->second;

        return constrFunction;
    } else {
        throw NativeScriptException("Constructor function not found for node: " + this->m_name);
    }
}

MetadataNode::TypeMetadata* MetadataNode::GetTypeMetadata(Isolate* isolate, const Local<Function>& value) {
    Local<Value> hiddenVal;
    V8GetPrivateValue(isolate, value, String::NewFromUtf8(isolate, "typemetadata").ToLocalChecked(), hiddenVal);

    auto data = reinterpret_cast<TypeMetadata*>(hiddenVal.As<External>()->Value());
    return data;
}

void MetadataNode::SetTypeMetadata(Isolate* isolate, Local<Function> value, TypeMetadata* data) {
    V8SetPrivateValue(isolate, value, String::NewFromUtf8(isolate, "typemetadata").ToLocalChecked(),  External::New(isolate, data));
}

MetadataNode* MetadataNode::GetInstanceMetadata(Isolate* isolate, const Local<Object>& value) {
    MetadataNode* node = nullptr;
    auto cache = GetMetadataNodeCache(isolate);
    auto key = Local<String>::New(isolate, *cache->MetadataKey);
    Local<Value> hiddenVal;
    V8GetPrivateValue(isolate, value, key, hiddenVal);
    auto ext = hiddenVal;
    if (!ext.IsEmpty()) {
        node = reinterpret_cast<MetadataNode*>(ext.As<External>()->Value());
    }

    return node;
}

void MetadataNode::SetInstanceMetadata(Isolate* isolate, Local<Object> object, MetadataNode* node) {
    auto cache = GetMetadataNodeCache(isolate);
    auto key = Local<String>::New(isolate, *cache->MetadataKey);
    V8SetPrivateValue(isolate, object, key, External::New(isolate, node));
}

void MetadataNode::ExtendedClassConstructorCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    TNSPERF();
    try {
        SET_PROFILER_FRAME();

        if (!info.IsConstructCall()) {
            throw NativeScriptException(string("Incorrectly calling a Java class as a method. Class must be created by invoking its constructor with the `new` keyword."));
        }

        auto isolate = info.GetIsolate();
        auto thiz = info.This();
        auto extData = reinterpret_cast<ExtendedClassCallbackData*>(info.Data().As<External>()->Value());

        v8::HandleScope handleScope(isolate);

        auto implementationObject = Local<Object>::New(isolate, *extData->implementationObject);

        SetInstanceMetadata(isolate, thiz, extData->node);
        thiz->SetInternalField(static_cast<int>(ObjectManager::MetadataNodeKeys::CallSuper), True(isolate));
        V8SetPrivateValue(isolate, thiz, V8StringConstants::GetImplementationObject(isolate), implementationObject);

        ArgsWrapper argWrapper(info, ArgType::Class);

        string fullClassName = extData->fullClassName;

        bool success = CallbackHandlers::RegisterInstance(isolate, thiz, fullClassName, argWrapper, implementationObject, false, extData->node->m_name);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::InterfaceConstructorCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    tns::instrumentation::Frame frame;
    try {
        SET_PROFILER_FRAME();

        if (!info.IsConstructCall()) {
            throw NativeScriptException(string("Interface implementation must be invoked as a constructor with the `new` keyword."));
        }

        auto isolate = info.GetIsolate();
        auto thiz = info.This();
        auto node = reinterpret_cast<MetadataNode*>(info.Data().As<External>()->Value());

        v8::HandleScope handleScope(isolate);

        Local<Object> implementationObject;
        Local<String> v8ExtendName;
        auto context = isolate->GetCurrentContext();

        if (info.Length() == 1) {
            if (!info[0]->IsObject()) {
                throw NativeScriptException(string("First argument must be implementation object"));
            }
            implementationObject = info[0]->ToObject(context).ToLocalChecked();
        } else if (info.Length() == 2) {
            if (!info[0]->IsString()) {
                throw NativeScriptException(string("First argument must be string"));
            }
            if (!info[1]->IsObject()) {
                throw NativeScriptException(string("Second argument must be implementation object"));
            }

            v8ExtendName = info[0]->ToString(context).ToLocalChecked();
            implementationObject = info[1]->ToObject(context).ToLocalChecked();
        } else {
            throw NativeScriptException(string("Invalid number of arguments"));
        }

        auto className = node->m_implType;
        SetInstanceMetadata(isolate, thiz, node);

        //@@@ Refactor
        thiz->SetInternalField(static_cast<int>(ObjectManager::MetadataNodeKeys::CallSuper), True(isolate));

        implementationObject->SetPrototype(context, thiz->GetPrototype());
        thiz->SetPrototype(context, implementationObject);
        V8SetPrivateValue(isolate, thiz, V8StringConstants::GetImplementationObject(isolate), implementationObject);

        ArgsWrapper argWrapper(info, ArgType::Interface);

        auto success = CallbackHandlers::RegisterInstance(isolate, thiz, className, argWrapper, implementationObject, true);

        if (frame.check()) {
            frame.log("Interface constructor: " + node->m_name);
        }
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::ClassConstructorCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    TNSPERF();
    try {
        SET_PROFILER_FRAME();

        auto thiz = info.This();

        auto isolate = info.GetIsolate();
        auto node = reinterpret_cast<MetadataNode*>(info.Data().As<External>()->Value());

        string extendName;
        auto className = node->m_name;

        SetInstanceMetadata(isolate, thiz, node);

        ArgsWrapper argWrapper(info, ArgType::Class);

        string fullClassName = CreateFullClassName(className, extendName);
        bool success = CallbackHandlers::RegisterInstance(isolate, thiz, fullClassName, argWrapper, Local<Object>(), false, className);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::MethodCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    try {
        SET_PROFILER_FRAME();

        auto e = info.Data().As<External>();

        auto callbackData = reinterpret_cast<MethodCallbackData*>(e->Value());
        auto initialCallbackData = reinterpret_cast<MethodCallbackData*>(e->Value());

        // Number of arguments the method is invoked with
        int argLength = info.Length();

        MetadataEntry* entry = nullptr;

        string* className;
        const auto& first = callbackData->candidates.front();
        const auto& methodName = first.name;

        while ((callbackData != nullptr) && (entry == nullptr)) {
            auto& candidates = callbackData->candidates;

            className = &callbackData->node->m_name;

            // Iterates through all methods and finds the best match based on the number of arguments
            auto found = false;
            for (auto& c : candidates) {
                found = (!c.isExtensionFunction && c.paramCount == argLength) || (
                        c.isExtensionFunction && c.paramCount == argLength + 1);
                if (found) {
                    if(c.isExtensionFunction){
                        className = &c.declaringType;
                    }
                    entry = &c;
                    DEBUG_WRITE("MetaDataEntry Method %s's signature is: %s", entry->name.c_str(), entry->sig.c_str());
                    break;
                }
            }

            // Iterates through the parent class's methods to find a good match
            if (!found) {
                callbackData = callbackData->parent;
            }
        }

        auto thiz = info.This();

        auto isSuper = false;
        if (!first.isStatic) {
            auto superValue = thiz->GetInternalField(static_cast<int>(ObjectManager::MetadataNodeKeys::CallSuper));
            isSuper = !superValue.IsEmpty() && superValue->IsTrue();
        }

        if ((argLength == 0) && (methodName == V8StringConstants::VALUE_OF)) {
            info.GetReturnValue().Set(thiz);
        } else {
            bool isFromInterface = initialCallbackData->node->IsNodeTypeInterface();
            CallbackHandlers::CallJavaMethod(thiz, *className, methodName, entry, isFromInterface, first.isStatic, isSuper, info);
        }
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::ArrayIndexedPropertyGetterCallback(uint32_t index, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto isolate = info.GetIsolate();
        auto context = isolate->GetCurrentContext();

        auto node = GetNodeFromHandle(thiz);

        auto element = CallbackHandlers::GetArrayElement(context, thiz, index, node->m_name);

        info.GetReturnValue().Set(element);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

void MetadataNode::ArrayIndexedPropertySetterCallback(uint32_t index, Local<Value> value, const PropertyCallbackInfo<Value>& info) {
    try {
        auto thiz = info.This();
        auto isolate = info.GetIsolate();
        auto context = isolate->GetCurrentContext();

        auto node = GetNodeFromHandle(thiz);

        CallbackHandlers::SetArrayElement(context, thiz, index, node->m_name, value);

        info.GetReturnValue().Set(value);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

Local<Object> MetadataNode::GetImplementationObject(Isolate* isolate, const Local<Object>& object) {
    TNSPERF();
    DEBUG_WRITE("GetImplementationObject called  on object:%d", object->GetIdentityHash());

    auto target = object;
    Local<Value> currentPrototype = target;

    Local<Object> implementationObject;

    Local<Value> hiddenVal;
    V8GetPrivateValue(isolate, object, V8StringConstants::GetImplementationObject(isolate), hiddenVal);
    implementationObject = hiddenVal.As<Object>();

    if (!implementationObject.IsEmpty()) {
        return implementationObject;
    }

    auto context = object->CreationContext();
    if (object->HasOwnProperty(context, V8StringConstants::GetIsPrototypeImplementationObject(isolate)).ToChecked()) {
        auto v8Prototype = V8StringConstants::GetPrototype(isolate);
        auto maybeHasOwnProperty = object->HasOwnProperty(context, v8Prototype);
        if (!object->HasOwnProperty(context, v8Prototype).ToChecked()) {
            return Local<Object>();
        }

        DEBUG_WRITE("GetImplementationObject returning the prototype of the object :%d", object->GetIdentityHash());
        Local<Value> result;
        if (object->Get(context, v8Prototype).ToLocal(&result)) {
            return result.As<Object>();
        } else {
            return Local<Object>();
        }
    }

    Local<Value> hiddenValue;
    V8GetPrivateValue(isolate, object, String::NewFromUtf8(isolate, "t::ActivityImplementationObject").ToLocalChecked(), hiddenValue);
    auto obj = hiddenValue.As<Object>();
    if (!obj.IsEmpty()) {
        DEBUG_WRITE("GetImplementationObject returning ActivityImplementationObject property on object: %d", object->GetIdentityHash());
        return obj;
    }

    Local<Value> lastPrototype;
    bool prototypeCycleDetected = false;
    while (implementationObject.IsEmpty()) {
        //
        currentPrototype = currentPrototype.As<Object>()->GetPrototype();

        if (currentPrototype->IsNull()) {
            break;
        }

        //DEBUG_WRITE("GetImplementationObject currentPrototypeObject:%d", (currentPrototype.IsEmpty() || currentPrototype.As<Object>().IsEmpty()) ? -1 :  currentPrototype.As<Object>()->GetIdentityHash());
        //DEBUG_WRITE("GetImplementationObject lastPrototypeObject:%d", (lastPrototype.IsEmpty() || lastPrototype.As<Object>().IsEmpty()) ? -1 :  lastPrototype.As<Object>()->GetIdentityHash());

        if (currentPrototype == lastPrototype) {
            auto abovePrototype = currentPrototype.As<Object>()->GetPrototype();
            prototypeCycleDetected = abovePrototype == currentPrototype;
        }

        if (currentPrototype.IsEmpty() || prototypeCycleDetected) {
            //Local<Value> abovePrototype = currentPrototype.As<Object>()->GetPrototype();
            //DEBUG_WRITE("GetImplementationObject not found since cycle parents reached abovePrototype:%d", (abovePrototype.IsEmpty() || abovePrototype.As<Object>().IsEmpty()) ? -1 :  abovePrototype.As<Object>()->GetIdentityHash());
            return Local<Object>();
        } else {
            Local<Value> hiddenVal;
            V8GetPrivateValue(isolate, currentPrototype.As<Object>(), V8StringConstants::GetClassImplementationObject(isolate), hiddenVal);
            auto value = hiddenVal;

            if (!value.IsEmpty()) {
                implementationObject = currentPrototype.As<Object>();
            }
        }

        lastPrototype = currentPrototype;
    }

    return implementationObject;
}

void MetadataNode::PackageGetterCallback(Local<Name> property, const PropertyCallbackInfo<Value>& info) {
    try {
        if (property.IsEmpty() || !property->IsString()) {
            return;
        }

        auto strProperty = property.As<String>();

        string propName = ArgConverter::ConvertToString(strProperty);

        if (propName.empty()) {
            return;
        }

        auto isolate = info.GetIsolate();

        auto thiz = info.This();

        Local<Value> hiddenVal;
        V8GetPrivateValue(isolate, thiz, strProperty, hiddenVal);
        auto cachedItem = hiddenVal;

        if (cachedItem.IsEmpty()) {
            auto node = reinterpret_cast<MetadataNode*>(info.Data().As<External>()->Value());

            uint8_t nodeType = s_metadataReader.GetNodeType(node->m_treeNode);

            DEBUG_WRITE("MetadataNode::GetterCallback: prop '%s' for node '%s' called, nodeType=%d, hash=%d", propName.c_str(), node->m_treeNode->name.c_str(), nodeType, thiz.IsEmpty() ? -42 : thiz->GetIdentityHash());

            auto child = GetChildMetadataForPackage(node, propName);
            auto foundChild = child.treeNode != nullptr;

            if (foundChild) {
                auto childNode = MetadataNode::GetOrCreateInternal(child.treeNode);
                cachedItem = childNode->CreateWrapper(isolate);

                uint8_t childNodeType = s_metadataReader.GetNodeType(child.treeNode);
                bool isInterface = s_metadataReader.IsNodeTypeInterface(childNodeType);
                if (isInterface) {
                    // For all java interfaces we register the special Symbol.hasInstance property
                    // which is invoked by the instanceof operator (https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Symbol/hasInstance).
                    // For example:
                    //
                    // Object.defineProperty(android.view.animation.Interpolator, Symbol.hasInstance, {
                    //    value: function(obj) {
                    //        return true;
                    //    }
                    // });
                    RegisterSymbolHasInstanceCallback(isolate, child, cachedItem);
                }

                V8SetPrivateValue(isolate, thiz, strProperty, cachedItem);

                if (node->m_name == "org/json" && child.name == "JSONObject") {
                    JSONObjectHelper::RegisterFromFunction(isolate, cachedItem);
                }
            }
        }

        info.GetReturnValue().Set(cachedItem);
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

bool MetadataNode::ValidateExtendArguments(const FunctionCallbackInfo<Value>& info, bool extendLocationFound, string& extendLocation, v8::Local<v8::String>& extendName, Local<Object>& implementationObject, bool isTypeScriptExtend) {
    auto isolate = info.GetIsolate();

    if (info.Length() == 1) {
        if (!extendLocationFound) {
            stringstream ss;
            ss << "Invalid extend() call. No name specified for extend at location: " << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        if (!info[0]->IsObject()) {
            stringstream ss;
            ss << "Invalid extend() call. No implementation object specified at location: " << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        auto context = isolate->GetCurrentContext();
        implementationObject = info[0]->ToObject(context).ToLocalChecked();
    } else if (info.Length() == 2 || isTypeScriptExtend) {
        if (!info[0]->IsString()) {
            stringstream ss;
            ss << "Invalid extend() call. No name for extend specified at location: " << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        if (!info[1]->IsObject()) {
            stringstream ss;
            ss << "Invalid extend() call. Named extend should be called with second object parameter containing overridden methods at location: " << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        DEBUG_WRITE("ExtendsCallMethodHandler: getting extend name");
        auto context = isolate->GetCurrentContext();
        extendName = info[0]->ToString(context).ToLocalChecked();
        bool isValidExtendName = IsValidExtendName(extendName);
        if (!isValidExtendName) {
            stringstream ss;
            ss << "The extend name \"" << ArgConverter::ConvertToString(extendName) << "\" you provided contains invalid symbols. Try using the symbols [a-z, A-Z, 0-9, _]." << endl;
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }
        implementationObject = info[1]->ToObject(context).ToLocalChecked();
    } else {
        stringstream ss;
        ss << "Invalid extend() call at location: " << extendLocation.c_str();
        string exceptionMessage = ss.str();

        throw NativeScriptException(exceptionMessage);
    }

    return true;
}

MetadataNode::ExtendedClassCacheData MetadataNode::GetCachedExtendedClassData(Isolate* isolate, const string& proxyClassName) {
    auto cache = GetMetadataNodeCache(isolate);
    ExtendedClassCacheData cacheData;
    auto itFound = cache->ExtendedCtorFuncCache.find(proxyClassName);
    if (itFound != cache->ExtendedCtorFuncCache.end()) {
        cacheData = itFound->second;
    }

    return cacheData;
}

string MetadataNode::CreateFullClassName(const std::string& className, const std::string& extendNameAndLocation = "") {
    string fullClassName = className;

    // create a class name consisting only of the base class name + last file name part + line + column + variable identifier
    if (!extendNameAndLocation.empty()) {
        string tempClassName = className;
        fullClassName = Util::ReplaceAll(tempClassName, "$", "_");
        fullClassName += "_" + extendNameAndLocation;
    }

    return fullClassName;
}

void MetadataNode::ExtendMethodCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    tns::instrumentation::Frame frame;
    try {
        if (info.IsConstructCall()) {
            string exMsg("Can't call 'extend' as constructor");
            throw NativeScriptException(exMsg);
        }

        SET_PROFILER_FRAME();

        Local<Object> implementationObject;
        Local<String> extendName;
        string extendLocation;

        auto hasDot = false;
        auto isTypeScriptExtend = false;
        auto isolate = info.GetIsolate();
        if (info.Length() == 2) {
            if (info[0].IsEmpty() || !info[0]->IsString()) {
                stringstream ss;
                ss << "Invalid extend() call. No name for extend specified at location: " << extendLocation.c_str();
                string exceptionMessage = ss.str();

                throw NativeScriptException(exceptionMessage);
            }
            if (info[1].IsEmpty() || !info[1]->IsObject()) {
                stringstream ss;
                ss << "Invalid extend() call. Named extend should be called with second object parameter containing overridden methods at location: " << extendLocation.c_str();
                string exceptionMessage = ss.str();

                throw NativeScriptException(exceptionMessage);
            }
            string strName = ArgConverter::ConvertToString(info[0].As<String>());
            hasDot = strName.find('.') != string::npos;
        } else if (info.Length() == 3) {
            auto context = isolate->GetCurrentContext();
            if (info[2]->IsBoolean() && info[2]->BooleanValue(isolate)) {
                isTypeScriptExtend = true;
            }
        }

        if (hasDot) {
            extendName = info[0].As<String>();
            implementationObject = info[1].As<Object>();
        } else {
            auto isValidExtendLocation = GetExtendLocation(isolate, extendLocation, isTypeScriptExtend);
            auto validArgs = ValidateExtendArguments(info, isValidExtendLocation, extendLocation, extendName, implementationObject, isTypeScriptExtend);

            if (!validArgs) {
                return;
            }
        }

        auto node = reinterpret_cast<MetadataNode*>(info.Data().As<External>()->Value());

        DEBUG_WRITE("ExtendsCallMethodHandler: called with %s", ArgConverter::ConvertToString(extendName).c_str());

        string extendNameAndLocation = extendLocation + ArgConverter::ConvertToString(extendName);
        string fullClassName;
        string baseClassName = node->m_name;
        if (!hasDot) {
            fullClassName = TNS_PREFIX + CreateFullClassName(baseClassName, extendNameAndLocation);
        } else {
            fullClassName = ArgConverter::ConvertToString(info[0].As<String>());
        }

        //resolve class (pre-generated or generated runtime from dex generator)
        uint8_t nodeType = s_metadataReader.GetNodeType(node->m_treeNode);
        bool isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);
        auto clazz = CallbackHandlers::ResolveClass(isolate, baseClassName, fullClassName, implementationObject, isInterface);
        auto fullExtendedName = CallbackHandlers::ResolveClassName(isolate, clazz);
        DEBUG_WRITE("ExtendsCallMethodHandler: extend full name %s", fullClassName.c_str());

        auto cachedData = GetCachedExtendedClassData(isolate, fullExtendedName);
        if (cachedData.extendedCtorFunction != nullptr) {
            auto cachedExtendedCtorFunc = Local<Function>::New(isolate, *cachedData.extendedCtorFunction);
            info.GetReturnValue().Set(cachedExtendedCtorFunc);
            return;
        }

        auto implementationObjectPropertyName = V8StringConstants::GetClassImplementationObject(isolate);
        //reuse validation - checks that implementationObject is not reused for different classes
        Local<Value> hiddenVal;
        V8GetPrivateValue(isolate, implementationObject, implementationObjectPropertyName, hiddenVal);
        auto implementationObjectProperty = hiddenVal.As<String>();
        if (implementationObjectProperty.IsEmpty()) {
            //mark the implementationObject as such and set a pointer to it's class node inside it for reuse validation later
            V8SetPrivateValue(isolate, implementationObject, implementationObjectPropertyName, String::NewFromUtf8(isolate, fullExtendedName.c_str()).ToLocalChecked());
        } else {
            string usedClassName = ArgConverter::ConvertToString(implementationObjectProperty);
            stringstream s;
            s << "This object is used to extend another class '" << usedClassName << "'";
            throw NativeScriptException(s.str());
        }

        auto baseClassCtorFunc = node->GetConstructorFunction(isolate);
        auto extendData = External::New(isolate, new ExtendedClassCallbackData(node, extendNameAndLocation, implementationObject, fullExtendedName));
        auto extendFuncTemplate = FunctionTemplate::New(isolate, ExtendedClassConstructorCallback, extendData);
        extendFuncTemplate->InstanceTemplate()->SetInternalFieldCount(static_cast<int>(ObjectManager::MetadataNodeKeys::END));

        auto context = isolate->GetCurrentContext();
        auto extendFunc = extendFuncTemplate->GetFunction(context).ToLocalChecked();
        auto prototypeName = V8StringConstants::GetPrototype(isolate);
        implementationObject->SetPrototype(context, baseClassCtorFunc->Get(context, prototypeName).ToLocalChecked());
        implementationObject->SetAccessor(context, V8StringConstants::GetSuper(isolate), SuperAccessorGetterCallback, nullptr, implementationObject);

        auto extendFuncPrototype = extendFunc->Get(context, prototypeName).ToLocalChecked().As<Object>();
        auto p = extendFuncPrototype->GetPrototype();
        extendFuncPrototype->SetPrototype(context, implementationObject);
        extendFunc->SetPrototype(context, baseClassCtorFunc);

        SetClassAccessor(extendFunc);
        SetTypeMetadata(isolate, extendFunc, new TypeMetadata(fullExtendedName));
        info.GetReturnValue().Set(extendFunc);

        s_name2NodeCache.insert(make_pair(fullExtendedName, node));

        ExtendedClassCacheData cacheData(extendFunc, fullExtendedName, node);
        auto cache = GetMetadataNodeCache(isolate);
        cache->ExtendedCtorFuncCache.insert(make_pair(fullExtendedName, cacheData));

        if (frame.check()) {
            frame.log("Extending: " + node->m_name);
        }
    } catch (NativeScriptException& e) {
        e.ReThrowToV8();
    } catch (std::exception e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToV8();
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToV8();
    }
}

bool MetadataNode::IsValidExtendName(const Local<String>& name) {
    string extendNam = ArgConverter::ConvertToString(name);

    for (int i = 0; i < extendNam.size(); i++) {
        char currentSymbol = extendNam[i];
        bool isValidExtendNameSymbol = isalpha(currentSymbol) ||
                                       isdigit(currentSymbol) ||
                                       currentSymbol == '_';
        if (!isValidExtendNameSymbol) {
            return false;
        }
    }

    return true;
}

bool MetadataNode::GetExtendLocation(v8::Isolate* isolate, string& extendLocation, bool isTypeScriptExtend) {
    stringstream extendLocationStream;
    auto stackTrace = StackTrace::CurrentStackTrace(Isolate::GetCurrent(), 3, StackTrace::kOverview);
    if (!stackTrace.IsEmpty()) {
        Local<StackFrame> frame;
        if (isTypeScriptExtend) {
            frame = stackTrace->GetFrame(isolate, 2); // the _super.apply call to ts_helpers will always be the third call frame
        }  else {
            frame = stackTrace->GetFrame(isolate, 0);
        }

        if (!frame.IsEmpty()) {
            auto scriptName = frame->GetScriptName();
            if (scriptName.IsEmpty()) {
                extendLocationStream << "unknown_location";
                extendLocation = extendLocationStream.str();
                return true;
            }

            string srcFileName = ArgConverter::ConvertToString(scriptName);
            // trim 'file://' to normalize path to always begin with "/data/"
            srcFileName = Util::ReplaceAll(srcFileName, "file://", "");

            string fullPathToFile;
            if (srcFileName == "<embedded>") {
                // Corner case, extend call is coming from the heap snapshot script
                // This is possible for lazily compiled code - e.g. from the body of a function

                // Replace embedded_script with 'script' as the SBG will emit classes from the
                // embedded_script file as 'Object_script_line_col', getting rid of fragments
                // preceding the underscore (_)
                fullPathToFile = "script";
            } else {
                string hardcodedPathToSkip = Constants::APP_ROOT_FOLDER_PATH;

                int startIndex = hardcodedPathToSkip.length();
                int strToTakeLen = (srcFileName.length() - startIndex - 3); // 3 refers to .js at the end of file name
                fullPathToFile = srcFileName.substr(startIndex, strToTakeLen);

                std::replace(fullPathToFile.begin(), fullPathToFile.end(), '/', '_');
                std::replace(fullPathToFile.begin(), fullPathToFile.end(), '.', '_');
                std::replace(fullPathToFile.begin(), fullPathToFile.end(), '-', '_');
                std::replace(fullPathToFile.begin(), fullPathToFile.end(), ' ', '_');

                std::vector<std::string> pathParts;

                Util::SplitString(fullPathToFile, "_", pathParts);

                std::string lastPathPart = pathParts.back();
                fullPathToFile = lastPathPart;
            }

            int lineNumber = frame->GetLineNumber();
            if (lineNumber < 0) {
                extendLocationStream << fullPathToFile.c_str() << " unkown line number";
                extendLocation = extendLocationStream.str();
                return false;
            }

            int column = frame->GetColumn();
            if (column < 0) {
                extendLocationStream << fullPathToFile.c_str() << " line:" << lineNumber << " unkown column number";
                extendLocation = extendLocationStream.str();
                return false;
            }

            // Account for the column length offset added by the addition of the Common JS function wrapper
            // See issue https://github.com/NativeScript/android-runtime/issues/975
            if (lineNumber == 1) {
                column = column - ModuleInternal::MODULE_PROLOGUE_LENGTH;
            }

            extendLocationStream << fullPathToFile.c_str() << "_" << lineNumber << "_" << column << "_";
        }
    }

    extendLocation = extendLocationStream.str();
    return true;
}

MetadataNode* MetadataNode::GetNodeFromHandle(const Local<Object>& value) {
    auto node = GetInstanceMetadata(Isolate::GetCurrent(), value);
    return node;
}

MetadataEntry MetadataNode::GetChildMetadataForPackage(MetadataNode* node, const string& propName) {
    MetadataEntry child;

    assert(node->m_treeNode->children != nullptr);

    const auto& children = *node->m_treeNode->children;

    for (auto treeNodeChild : children) {
        if (propName == treeNodeChild->name) {
            child.name = propName;
            child.treeNode = treeNodeChild;

            uint8_t childNodeType = s_metadataReader.GetNodeType(treeNodeChild);
            if (s_metadataReader.IsNodeTypeInterface(childNodeType)) {
                bool isPrefix;
                string declaringType = s_metadataReader.ReadInterfaceImplementationTypeName(treeNodeChild, isPrefix);
                child.declaringType = isPrefix
                                      ? (declaringType + s_metadataReader.ReadTypeName(child.treeNode))
                                      : declaringType;
            }
        }
    }

    return child;
}

void MetadataNode::BuildMetadata(const string& filesPath) {
    timeval time1;
    gettimeofday(&time1, nullptr);

    string baseDir = filesPath;
    baseDir.append("/metadata");

    DIR* dir = opendir(baseDir.c_str());

    if(dir == nullptr){
        stringstream ss;
        ss << "metadata folder couldn't be opened! (Error: ";
        ss << errno;
        ss << ") ";

        // TODO: Is there a way to detect if the screen is locked as verification
        // We assume based on the error that this is the only way to get this specific error here at this point
        if (errno == ENOENT || errno == EACCES) {
            // Log the error with error code
            __android_log_print(ANDROID_LOG_ERROR, "TNS.error", "%s", ss.str().c_str());

            // While the screen is locked after boot; we cannot access our own apps directory on Android 9+
            // So the only thing to do at this point is just exit normally w/o crashing!

            // The only reason we should be in this specific path; is if:
            // 1) android:directBootAware="true" flag is set on receiver
            // 2) android.intent.action.LOCKED_BOOT_COMPLETED intent is set in manifest on above receiver
            // See:  https://developer.android.com/guide/topics/manifest/receiver-element
            //  and: https://developer.android.com/training/articles/direct-boot
            // This specific path occurs if you using the NativeScript-Local-Notification plugin, the
            // receiver code runs fine, but the app actually doesn't need to startup.  The Native code tries to
            // startup because the receiver is triggered.  So even though we are exiting, the receiver will have
            // done its job

            exit(0);
        }
        else {
          throw NativeScriptException(ss.str());
        }
    }

    string nodesFile = baseDir + "/treeNodeStream.dat";
    string namesFile = baseDir + "/treeStringsStream.dat";
    string valuesFile = baseDir + "/treeValueStream.dat";

    FILE* f = fopen(nodesFile.c_str(), "rb");
    if (f == nullptr) {
        stringstream ss;
        ss << "metadata file (treeNodeStream.dat) couldn't be opened! (Error: ";
        ss << errno;
        ss << ") ";

        throw NativeScriptException(ss.str());
    }
    fseek(f, 0, SEEK_END);
    int lenNodes = ftell(f);
    assert((lenNodes % sizeof(MetadataTreeNodeRawData)) == 0);
    char* nodes = new char[lenNodes];
    rewind(f);
    fread(nodes, 1, lenNodes, f);
    fclose(f);

    const int _512KB = 524288;

    f = fopen(namesFile.c_str(), "rb");
    if (f == nullptr) {
        stringstream ss;
        ss << "metadata file (treeStringsStream.dat) couldn't be opened! (Error: ";
        ss << errno;
        ss << ") ";
        throw NativeScriptException(ss.str());
    }
    fseek(f, 0, SEEK_END);
    int lenNames = ftell(f);
    char* names = new char[lenNames + _512KB];
    rewind(f);
    fread(names, 1, lenNames, f);
    fclose(f);

    f = fopen(valuesFile.c_str(), "rb");
    if (f == nullptr) {
        stringstream ss;
        ss << "metadata file (treeValueStream.dat) couldn't be opened! (Error: ";
        ss << errno;
        ss << ") ";
        throw NativeScriptException(ss.str());
    }
    fseek(f, 0, SEEK_END);
    int lenValues = ftell(f);
    char* values = new char[lenValues + _512KB];
    rewind(f);
    fread(values, 1, lenValues, f);
    fclose(f);

    timeval time2;
    gettimeofday(&time2, nullptr);

    DEBUG_WRITE("lenNodes=%d, lenNames=%d, lenValues=%d", lenNodes, lenNames, lenValues);

    long millis1 = (time1.tv_sec * 1000) + (time1.tv_usec / 1000);
    long millis2 = (time2.tv_sec * 1000) + (time2.tv_usec / 1000);

    DEBUG_WRITE("time=%ld", (millis2 - millis1));

    BuildMetadata(lenNodes, reinterpret_cast<uint8_t*>(nodes), lenNames, reinterpret_cast<uint8_t*>(names), lenValues, reinterpret_cast<uint8_t*>(values));

    delete[] nodes;
    //delete[] names;
    //delete[] values;
}

void MetadataNode::BuildMetadata(uint32_t nodesLength, uint8_t* nodeData, uint32_t nameLength, uint8_t* nameData, uint32_t valueLength, uint8_t* valueData) {
    s_metadataReader = MetadataReader(nodesLength, nodeData, nameLength, nameData, valueLength, valueData, CallbackHandlers::GetTypeMetadata);
}

void MetadataNode::CreateTopLevelNamespaces(Isolate* isolate, const Local<Object>& global) {
    auto context = isolate->GetCurrentContext();
    auto root = s_metadataReader.GetRoot();

    const auto& children = *root->children;

    for (auto treeNode : children) {
        uint8_t nodeType = s_metadataReader.GetNodeType(treeNode);

        if (nodeType == MetadataTreeNode::PACKAGE) {
            auto node = GetOrCreateInternal(treeNode);

            auto packageObj = node->CreateWrapper(isolate);
            string nameSpace = node->m_treeNode->name;
            // if the namespaces matches a javascript keyword, prefix it with $ to avoid TypeScript and JavaScript errors
            if (IsJavascriptKeyword(nameSpace)) {
                nameSpace = "$" + nameSpace;
            }
            global->Set(context, ArgConverter::ConvertToV8String(isolate, nameSpace), packageObj);
        }
    }
}

MetadataNode::MetadataNodeCache* MetadataNode::GetMetadataNodeCache(Isolate* isolate) {
    MetadataNodeCache* cache;
    auto itFound = s_metadata_node_cache.find(isolate);
    if (itFound == s_metadata_node_cache.end()) {
        cache = new MetadataNodeCache;
        s_metadata_node_cache.insert(make_pair(isolate, cache));
    } else {
        cache = itFound->second;
    }
    return cache;
}

void MetadataNode::EnableProfiler(bool enableProfiler) {
    s_profilerEnabled = enableProfiler;
}

bool MetadataNode::IsJavascriptKeyword(std::string word) {
    static set<string> keywords;

    if (keywords.empty()) {
        string kw[] { "abstract", "arguments", "boolean", "break", "byte", "case", "catch", "char", "class", "const", "continue", "debugger", "default", "delete", "do",
                      "double", "else", "enum", "eval", "export", "extends", "false", "final", "finally", "float", "for", "function", "goto", "if", "implements",
                      "import", "in", "instanceof", "int", "interface", "let", "long", "native", "new", "null", "package", "private", "protected", "public", "return",
                      "short", "static", "super", "switch", "synchronized", "this", "throw", "throws", "transient", "true", "try", "typeof", "var", "void", "volatile", "while", "with", "yield"
                    };

        keywords = set<string>(kw, kw + sizeof(kw)/sizeof(kw[0]));
    }

    return keywords.find(word) != keywords.end();
}

Local<Function> MetadataNode::Wrap(Isolate* isolate, const Local<Function>& function, const string& name, const string& origin, bool isCtorFunc) {
    if (!s_profilerEnabled || name == "<init>") {
        return function;
    }

    string actualName = name;
    while (IsJavascriptKeyword(actualName)) {
        actualName.append("_");
    }

    Local<Function> ret;

    stringstream ss;
    ss << "(function() { ";
    ss << "function " << actualName << "() { ";
    if (isCtorFunc) {
        ss << "var args = [null]; for (var i=0; i<arguments.length; i++) { args.push(arguments[i]); }; ";
        ss << "return new (Function.prototype.bind.apply(" << actualName << ".__func, args)); ";
    } else {
        ss << "return " << actualName << ".__func.apply(this, arguments); ";
    }
    ss << "} ";
    ss << "return " << actualName << "; ";
    ss << "})()";

    auto str = ss.str();
    auto source = ArgConverter::ConvertToV8String(isolate, str);
    auto context = isolate->GetCurrentContext();

    TryCatch tc(isolate);

    Local<Script> script;
    ScriptOrigin jsOrigin(ArgConverter::ConvertToV8String(isolate, origin));
    auto maybeScript = Script::Compile(context, source, &jsOrigin).ToLocal(&script);

    if (tc.HasCaught()) {
        throw NativeScriptException(tc, "Cannot compile wrapper");
    }

    if (!script.IsEmpty()) {
        Local<Value> result;
        auto maybeResult = script->Run(context).ToLocal(&result);

        if (!result.IsEmpty()) {
            ret = result.As<Function>();
            ret->Set(context, ArgConverter::ConvertToV8String(isolate, "__func"), function);
            ret->SetName(ArgConverter::ConvertToV8String(isolate, actualName));

            auto prototypePropName = V8StringConstants::GetPrototype(isolate);
            ret->Set(context, prototypePropName, function->Get(context, prototypePropName).ToLocalChecked());
        } else {
            throw NativeScriptException("Cannot create wrapper function");
        }
    } else {
        throw NativeScriptException(str);
    }

    return ret;
}

bool MetadataNode::CheckClassHierarchy(JEnv& env, jclass currentClass, MetadataTreeNode* curentTreeNode, MetadataTreeNode* baseTreeNode, std::vector<MetadataTreeNode*>& skippedBaseTypes) {
    auto shouldSkipBaseClass = false;
    if ((currentClass != nullptr) && (baseTreeNode != curentTreeNode) && (baseTreeNode != nullptr) &&
            (baseTreeNode->offsetValue > 0)) {
        auto baseNode = GetOrCreateInternal(baseTreeNode);
        auto baseClass = env.FindClass(baseNode->m_name);
        if (baseClass != nullptr) {
            auto isBaseClass = env.IsAssignableFrom(currentClass, baseClass) == JNI_TRUE;
            if (!isBaseClass) {
                skippedBaseTypes.push_back(baseTreeNode);
                shouldSkipBaseClass = true;
            }
        }
    }
    return shouldSkipBaseClass;
}

/*
 * This method handles scenrios like Bundle/BaseBundle class hierarchy change in API level 21.
 * See https://github.com/NativeScript/android-runtime/issues/628
 */
void MetadataNode::SetMissingBaseMethods(Isolate* isolate, const vector<MetadataTreeNode*>& skippedBaseTypes, const vector<MethodCallbackData*>& instanceMethodData, Local<ObjectTemplate>& prototypeTemplate) {
    for (auto treeNode: skippedBaseTypes) {
        uint8_t* curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;

        auto nodeType = s_metadataReader.GetNodeType(treeNode);

        auto curType = s_metadataReader.ReadTypeName(treeNode);

        curPtr += sizeof(uint16_t /* baseClassId */);

        if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
            curPtr += sizeof(uint8_t) + sizeof(uint32_t);
        }

        //get candidates from instance methods metadata
        auto instanceMethodCount = *reinterpret_cast<uint16_t*>(curPtr);
        curPtr += sizeof(uint16_t);
        MethodCallbackData* callbackData = nullptr;

        for (auto i = 0; i < instanceMethodCount; i++) {
            auto entry = s_metadataReader.ReadInstanceMethodEntry(&curPtr);

            auto isConstructor = entry.name == "<init>";
            if (isConstructor) {
                continue;
            }

            for (auto data: instanceMethodData) {
                if (data->candidates.front().name == entry.name) {
                    callbackData = data;
                    break;
                }
            }

            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);

                auto funcData = External::New(isolate, callbackData);
                auto funcTemplate = FunctionTemplate::New(isolate, MethodCallback, funcData);
                auto funcName = ArgConverter::ConvertToV8String(isolate, entry.name);
                prototypeTemplate->Set(funcName, funcTemplate);
            }

            bool foundSameSig = false;
            for (auto m: callbackData->candidates) {
                foundSameSig = m.sig == entry.sig;
                if (foundSameSig) {
                    break;
                }
            }

            if (!foundSameSig) {
                callbackData->candidates.push_back(entry);
            }
        }
    }
}

void MetadataNode::RegisterSymbolHasInstanceCallback(Isolate* isolate, MetadataEntry entry, Local<Value> interface) {
    if (interface->IsNullOrUndefined()) {
        return;
    }

    JEnv env;
    auto className = GetJniClassName(entry);
    auto clazz = env.FindClass(className);
    if (clazz == nullptr) {
        return;
    }

    auto extData = External::New(isolate, clazz);
    auto hasInstanceTemplate = FunctionTemplate::New(isolate, MetadataNode::SymbolHasInstanceCallback, extData);
    auto context = isolate->GetCurrentContext();
    auto hasInstanceFunc = hasInstanceTemplate->GetFunction(context).ToLocalChecked();
    PropertyDescriptor descriptor(hasInstanceFunc, false);
    auto hasInstanceSymbol = Symbol::GetHasInstance(isolate);
    interface->ToObject(context).ToLocalChecked()->DefineProperty(context, hasInstanceSymbol, descriptor);
}

void MetadataNode::SymbolHasInstanceCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto length = info.Length();
    if (length != 1) {
        throw NativeScriptException(string("Symbol.hasInstance must take exactly 1 argument"));
    }

    auto arg = info[0];
    if (arg->IsNullOrUndefined()) {
        info.GetReturnValue().Set(false);
        return;
    }

    auto clazz = reinterpret_cast<jclass>(info.Data().As<External>()->Value());

    auto isolate = info.GetIsolate();
    auto context = isolate->GetCurrentContext();
    auto runtime = Runtime::GetRuntime(isolate);
    auto objectManager = runtime->GetObjectManager();
    auto obj = objectManager->GetJavaObjectByJsObject(arg->ToObject(context).ToLocalChecked());

    if (obj.IsNull()) {
        // Couldn't find a corresponding java instance counterpart. This could happen
        // if the "instanceof" operator is invoked on a pure javascript instance
        info.GetReturnValue().Set(false);
        return;
    }

    JEnv env;
    auto isInstanceOf = env.IsInstanceOf(obj, clazz);

    info.GetReturnValue().Set(isInstanceOf);
}

std::string MetadataNode::GetJniClassName(MetadataEntry entry) {
    std::stack<string> s;
    MetadataTreeNode* n = entry.treeNode;
    while (n != nullptr && n->name != "") {
        s.push(n->name);
        n = n->parent;
    }

    string fullClassName;
    while (!s.empty()) {
        auto top = s.top();
        fullClassName = (fullClassName == "") ? top : fullClassName + "/" + top;
        s.pop();
    }

    return fullClassName;
}

void MetadataNode::onDisposeIsolate(Isolate* isolate) {
    {
        auto it = s_metadata_node_cache.find(isolate);
        if (it != s_metadata_node_cache.end()) {
            delete it->second;
            s_metadata_node_cache.erase(it);
        }
    }
    {
        auto it = s_arrayObjectTemplates.find(isolate);
        if (it != s_arrayObjectTemplates.end()) {
            delete it->second;
            s_arrayObjectTemplates.erase(it);
        }
    }
    {
        for (auto it = s_treeNode2NodeCache.begin(); it != s_treeNode2NodeCache.end(); it++) {
            auto it2 = it->second->m_poCtorCachePerIsolate.find(isolate);
            if(it2 != it->second->m_poCtorCachePerIsolate.end()) {
                delete it2->second;
                it->second->m_poCtorCachePerIsolate.erase(it2);
            }
        }
    }
}

string MetadataNode::TNS_PREFIX = "com/tns/gen/";
MetadataReader MetadataNode::s_metadataReader;
std::map<std::string, MetadataNode*> MetadataNode::s_name2NodeCache;
std::map<std::string, MetadataTreeNode*> MetadataNode::s_name2TreeNodeCache;
std::map<MetadataTreeNode*, MetadataNode*> MetadataNode::s_treeNode2NodeCache;
map<Isolate*, MetadataNode::MetadataNodeCache*> MetadataNode::s_metadata_node_cache;
bool MetadataNode::s_profilerEnabled = false;
std::map<Isolate*, Persistent<ObjectTemplate>*> MetadataNode::s_arrayObjectTemplates;

