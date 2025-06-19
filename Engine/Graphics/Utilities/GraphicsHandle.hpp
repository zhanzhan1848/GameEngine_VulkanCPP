#pragma once

#ifndef UNIQUE_GRAPHICS_RESOURCE_HPP
#define UNIQUE_GRAPHICS_RESOURCE_HPP

#include <memory>
#include <type_traits>
#include <cassert>
#include "CommonHeaders.h"

namespace primal::graphics
{
    // 通用资源句柄基类
    template<typename ResourceType, typename Deleter = std::default_delete<ResourceType>>
    class resource_handle
    {
    public:
        using resource_type = ResourceType;
        using pointer = ResourceType*;
        using unique_ptr_type = std::unique_ptr<ResourceType, Deleter>;

        DISABLE_COPY(resource_handle);
        
        resource_handle() = default;
        
        explicit resource_handle(pointer resource)
            : m_resource(resource)
        {
        }
        
        explicit resource_handle(unique_ptr_type resource)
            : m_resource(std::move(resource))
        {
        }
        
        // 允许移动
        resource_handle(resource_handle&& other) noexcept = default;
        resource_handle& operator=(resource_handle&& other) noexcept = default;
        
        // 析构时自动释放资源
        ~resource_handle() = default;
        
        // 获取原始资源
        [[nodiscard]] pointer get() const { return m_resource.get(); }
        
        // 判断资源是否有效
        [[nodiscard]] bool is_valid() const { return m_resource != nullptr; }
        
        // 显式释放资源
        void release() { m_resource.reset(); }
        
        // 重置资源
        void reset(pointer resource = nullptr) { m_resource.reset(resource); }
        
        // 访问运算符
        pointer operator->() const { return m_resource.get(); }
        ResourceType& operator*() const { return *m_resource; }
        
        // 隐式转换为bool
        explicit operator bool() const { return is_valid(); }
        
    protected:
        unique_ptr_type m_resource;
    };

    // 通用资源信息基类
    template<typename Derived, typename InfoType>
    class resource_info{
    public:
        DISABLE_COPY_AND_MOVE(resource_info);

        resource_info() : m_info(std::make_unique<InfoType>()) {}

        // 接受自定义构造参数
        template<typename... Args>
        resource_info(Args&&... args) : m_info(std::make_unique<InfoType>(std::forward<Args>(args)...)) {}

        // 链式设置字段
        template<typename FieldType>
        Derived& set_field(FieldType InfoType::* field, const FieldType& value) {
            m_info.*field = value;
            return static_cast<Derived&>(*this);
        }

        // 完成并验证
        const InfoType& finish() const {
            static_assert(std::is_same_v<bool, decltype(std::declval<Derived>().is_valid())>,
                          "Derived class must implement 'bool is_valid() const'");
            if (static_cast<const Derived*>(this)->is_valid()) {
                return m_info;
            }
            throw std::runtime_error("Failed to validate ResourceInfo");
        }

    protected:
        resource_info() = default;
        InfoType m_info{};
    };

    // 资源创建中间类
    template<typename ResourceType, typename InfoType>
    class resource_creator {
    public:
        resource_creator(const InfoType& info)
            : m_info(info), m_resource(nullptr) {}

        template<typename Deleter>
        auto create(Deleter&& deleter) && {
            create_resource();  // 特化实现创建逻辑
            check_resource();
            return resource_handle<ResourceType, std::decay_t<Deleter>>(
                m_resource.release(), std::forward<Deleter>(deleter)
            );
        }

    private:
        virtual void create_resource() {
            throw std::runtime_error("create_resource must be specialized for this ResourceType");
        }

        void check_resource() const {
            if (!m_resource) {
                throw std::runtime_error("Resource creation failed");
            }
        }

        InfoType m_info;
        std::unique_ptr<ResourceType, std::default_delete<ResourceType>> m_resource;
    };

    // ResourceInfo 工具类
    template<typename InfoType, typename InitFunc, typename ValidFunc>
    class resource_info_helper : public ResourceInfo<resource_info_helper<InfoType, InitFunc, ValidFunc>, InfoType> {
    public:
        resource_info_helper() { InitFunc()(this->m_info); }
        static bool is_valid(const InfoType& info) { return ValidFunc()(info); }
    };

    // ResourceCreator 工具类
    template<typename ResourceType, typename InfoType, typename CreateFunc>
    class resource_creator_helper : public ResourceCreator<ResourceType, InfoType> {
    public:
        using ResourceCreator<ResourceType, InfoType>::ResourceCreator;
    private:
        void create_resource() override { CreateFunc()(this->m_info, this->m_resource); }
    };
    
    // 使用自定义删除器的资源句柄工厂函数
    template<typename ResourceType, typename Deleter>
    auto make_resource_handle(ResourceType* resource, Deleter&& deleter)
    {
        return resource_handle<ResourceType, Deleter>(
            std::unique_ptr<ResourceType, Deleter>(resource, std::forward<Deleter>(deleter))
        );
    }

    // 工厂函数
    template<typename ResourceType, typename InfoType, typename Deleter>
    auto make_resource(const InfoType& info, Deleter&& deleter) {
        resource_creator<ResourceType, InfoType> creator(info);
        return std::move(creator).create(std::forward<Deleter>(deleter));
    }
}

#define DEFINE_RESOURCE_INFO_SIMPLE(ClassName, InfoType, InitCode, ValidCode) \
    using ClassName = primal::graphics::resource_info_helper< \
        InfoType, \
        std::function<void(InfoType&)>([](InfoType& info) { InitCode; }), \
        std::function<bool(const InfoType&)>([](const InfoType& info) { return ValidCode; }) \
    >;

#define DEFINE_RESOURCE_CREATOR_SIMPLE(ResourceType, InfoType, CreateCode) \
    using ResourceType##Creator = primal::graphics::resource_creator_helper< \
        ResourceType, InfoType, \
        std::function<void(const InfoType&, std::unique_ptr<ResourceType, std::default_delete<ResourceType>>&)>( \
            [](const InfoType& info, auto& resource) { CreateCode; } \
        ) \
    >;

#endif // UNIQUE_GRAPHICS_RESOURCE_HPP

#ifndef SHARED_GRAPHICS_RESOURCE_HPP
#define SHARED_GRAPHICS_RESOURCE_HPP

#include <memory>
#include <mutex>
#include <type_traits>
#include <stdexcept>
#include "CommonHeaders.h" // 假设包含基本类型和宏定义

namespace primal::graphics
{
    // 非唯一资源句柄基类
    template<typename ResourceType, typename Deleter = std::default_delete<ResourceType>>
    class shared_resource_handle
    {
    public:
        using resource_type = ResourceType;
        using pointer = ResourceType*;
        using shared_ptr_type = std::shared_ptr<ResourceType>;

        // 禁用拷贝，只允许移动
        DISABLE_COPY(shared_resource_handle);

        shared_resource_handle() = default;

        explicit shared_resource_handle(shared_ptr_type resource)
            : m_resource(std::move(resource)) {}

        // 移动构造和赋值
        shared_resource_handle(shared_resource_handle&& other) noexcept = default;
        shared_resource_handle& operator=(shared_resource_handle&& other) noexcept = default;

        // 获取原始资源（线程安全）
        [[nodiscard]] pointer get() const {
            std::shared_lock lock(m_mutex);
            return m_resource.get();
        }

        // 判断资源是否有效
        [[nodiscard]] bool is_valid() const {
            std::shared_lock lock(m_mutex);
            return m_resource != nullptr;
        }

        // 隐式转换为 bool
        explicit operator bool() const { return is_valid(); }

    protected:
        mutable std::shared_mutex m_mutex; // 支持读写锁
        shared_ptr_type m_resource;
    };

    // 资源信息基类（CRTP）
    template<typename Derived, typename InfoType>
    class shared_resource_info
    {
    public:
        // 禁用拷贝和移动（子类可自定义）
        DISABLE_COPY_AND_MOVE(shared_resource_info);

        // 链式设置字段
        template<typename FieldType>
        Derived& set_field(FieldType InfoType::* field, const FieldType& value) {
            std::lock_guard lock(m_mutex);
            m_info.*field = value;
            return static_cast<Derived&>(*this);
        }

        // 获取资源信息（线程安全）
        [[nodiscard]] InfoType get_info() const {
            std::shared_lock lock(m_mutex);
            return m_info;
        }

        // 验证资源信息
        [[nodiscard]] bool is_valid() const {
            std::shared_lock lock(m_mutex);
            return static_cast<const Derived*>(this)->validate();
        }

    protected:
        shared_resource_info() = default;
        mutable std::shared_mutex m_mutex;
        InfoType m_info{};
    };

    // 可编辑资源基类（CRTP）
    // 可编辑资源基类（去除Info依赖）
    template<typename Derived, typename ResourceType>
    class editable_resource : public shared_resource_handle<ResourceType>
    {
    public:
        editable_resource(std::shared_ptr<ResourceType> resource) 
            : shared_resource_handle<ResourceType>(std::move(resource)) {}

        // 编辑资源内容的线程安全接口
        template<typename EditorFunc>
        void edit(EditorFunc&& editor) {
            std::lock_guard lock(this->m_mutex);
            if (!this->m_resource) {
                throw std::runtime_error("Resource is invalid");
            }
            editor(this->m_resource.get());
        }
    };

    // 资源创建器
    template<typename ResourceType>
    class resource_creator
    {
    public:
        resource_creator() = default;

        template<typename Deleter>
        auto create(Deleter&& deleter) && {
            auto resource = std::make_shared<ResourceType>();
            create_resource(resource.get());
            check_resource(resource.get());
            return editable_resource<typename ResourceType::resource_type, ResourceType>(
                std::shared_ptr<ResourceType>(resource, std::forward<Deleter>(deleter))
            );
        }

    private:
        virtual void create_resource(ResourceType* resource) {
            throw std::runtime_error("create_resource must be specialized");
        }

        void check_resource(const ResourceType* resource) const {
            if (!resource) {
                throw std::runtime_error("Resource creation failed");
            }
        }
    };
}

// Texture 创建宏和元编程实现
#define DEFINE_TEXTURE_CREATOR(API_TYPE, RESOURCE_TYPE, DESC_TYPE, CREATE_CODE) \
template<> \
class TextureCreator<RESOURCE_TYPE> : public resource_creator<RESOURCE_TYPE> { \
public: \
    TextureCreator(const DESC_TYPE& desc) : m_desc(desc) {} \
private: \
    void create_resource(RESOURCE_TYPE* resource) override { \
        CREATE_CODE \
    } \
    DESC_TYPE m_desc; \
};

// Buffer 创建宏和元编程实现
#define DEFINE_BUFFER_CREATOR(API_TYPE, RESOURCE_TYPE, DESC_TYPE, CREATE_CODE) \
template<> \
class BufferCreator<RESOURCE_TYPE> : public resource_creator<RESOURCE_TYPE> { \
public: \
    BufferCreator(const DESC_TYPE& desc) : m_desc(desc) {} \
private: \
    void create_resource(RESOURCE_TYPE* resource) override { \
        CREATE_CODE \
    } \
    DESC_TYPE m_desc; \
};

#endif // SHARED_GRAPHICS_RESOURCE_HPP