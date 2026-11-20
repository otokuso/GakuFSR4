#pragma once

#include "ManagedRoot.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace gakufsr {
using P = void*;
using M = const void*;

struct ManagedFailure : std::runtime_error { using std::runtime_error::runtime_error; };

struct Api {
    P (*domain_get)();
    P* (*domain_get_assemblies)(P,size_t*);
    P (*assembly_get_image)(P);
    P (*class_from_name)(P,const char*,const char*);
    M (*class_get_methods)(P,P*);
    P (*class_get_parent)(P);
    const char* (*method_get_name)(M);
    uint32_t (*method_get_param_count)(M);
    P (*method_get_param)(M,uint32_t);
    char* (*type_get_name)(P);
    void (*free)(P);
    P (*class_get_field_from_name)(P,const char*);
    size_t (*field_get_offset)(P);
    int32_t (*class_value_size)(P,uint32_t*);
    P (*runtime_invoke)(M,P,P*,P*);
    P (*object_unbox)(P);
    P (*object_new)(P);
    P (*object_get_class)(P);
    GCHandle (*gchandle_new)(P,bool);
    P (*gchandle_get_target)(GCHandle);
    void (*gchandle_free)(GCHandle);
    P (*resolve_icall)(const char*);
    void (*format_exception)(P,char*,int);

    void Bind(HMODULE module) {
#define LOAD_API(n) n=reinterpret_cast<decltype(n)>(GetProcAddress(module,"il2cpp_" #n)); if(!n) throw std::runtime_error("Missing il2cpp_" #n)
        LOAD_API(domain_get); LOAD_API(domain_get_assemblies); LOAD_API(assembly_get_image);
        LOAD_API(class_from_name); LOAD_API(class_get_methods); LOAD_API(class_get_parent);
        LOAD_API(method_get_name); LOAD_API(method_get_param_count); LOAD_API(method_get_param);
        LOAD_API(type_get_name); LOAD_API(free); LOAD_API(class_get_field_from_name);
        LOAD_API(field_get_offset); LOAD_API(class_value_size); LOAD_API(runtime_invoke);
        LOAD_API(object_unbox); LOAD_API(object_new); LOAD_API(object_get_class);
        LOAD_API(gchandle_new); LOAD_API(gchandle_get_target); LOAD_API(gchandle_free);
        LOAD_API(resolve_icall);
        format_exception=reinterpret_cast<decltype(format_exception)>(GetProcAddress(module,"il2cpp_format_exception"));
#undef LOAD_API
    }
    P Class(const char* ns,const char* name) {
        size_t count=0;
        auto assemblies=domain_get_assemblies(domain_get(),&count);
        for(size_t i=0;i<count;++i)
            if(auto c=class_from_name(assembly_get_image(assemblies[i]),ns,name)) return c;
        throw std::runtime_error(std::string("Class not found: ")+ns+"."+name);
    }
    M Method(P c,const char* name,std::initializer_list<const char*> types) {
        M result=nullptr;
        for(P parent=c;parent && !result;parent=class_get_parent(parent)) {
            P iter=nullptr;
            while(auto m=class_get_methods(parent,&iter)) {
                if(strcmp(method_get_name(m),name) || method_get_param_count(m)!=types.size()) continue;
                bool match=true; uint32_t i=0;
                for(auto type:types) {
                    char* actual=type_get_name(method_get_param(m,i++));
                    if(!actual || strcmp(type,actual)) match=false;
                    if(actual) free(actual);
                }
                if(match) {
                    if(result) throw std::runtime_error(std::string("Ambiguous method: ")+name);
                    result=m;
                }
            }
        }
        if(!result) throw std::runtime_error(std::string("Method not found: ")+name);
        return result;
    }
    int Offset(P c,const char* name,bool value=false) {
        auto f=class_get_field_from_name(c,name);
        if(!f) throw std::runtime_error(std::string("Field not found: ")+name);
        auto offset=field_get_offset(f);
        if(offset<16 || offset>65536) throw std::runtime_error("Invalid field offset");
        return static_cast<int>(offset)-(value?16:0);
    }
    P Invoke(M m,P self,std::initializer_list<P> args={}) {
        P exception=nullptr;
        P result=runtime_invoke(m,self,const_cast<P*>(args.begin()),&exception);
        if(exception) {
            char detail[512]{};
            if(format_exception) format_exception(exception,detail,sizeof detail);
            detail[sizeof detail-1]='\0';
            throw ManagedFailure(std::string("Managed exception: ")+method_get_name(m)+": "+detail);
        }
        return result;
    }
    template<class T> T Value(M m,P self=nullptr,std::initializer_list<P> args={}) {
        P result=Invoke(m,self,args);
        if(!result) throw std::runtime_error("Missing boxed return value");
        return *static_cast<T*>(object_unbox(result));
    }
};
}
