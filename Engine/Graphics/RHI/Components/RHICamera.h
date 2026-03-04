#pragma once

#include "Engine/Common/CommonHeaders.h"
#include "Engine/EngineAPI/Input.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::rhi {

    /**
     * @brief RHI层相机组件
     * @details 提供基本的漫游相机功能，支持键盘移动和鼠标旋转
     */
    class RHICamera {
    public:
        RHICamera() = default;

        /**
         * @brief 初始化相机
         * @param pos 初始位置
         * @param rot 初始旋转 (Pitch, Yaw, Roll)
         */
        void Initialize(math::v3 pos, math::v3 rot) {
            position = pos;
            rotation = rot;
            UpdateVectors();
        }

        /**
         * @brief 更新相机状态
         * @param dt 帧时间间隔 (秒)
         */
        void Update(float dt) {
            HandleInput(dt);
        }

        /**
         * @brief 获取视图矩阵
         * @return 视图矩阵 (LookAt)
         */
        math::m4x4 GetViewMatrix() const {
            // Re-calculate target based on current position and forward vector
            math::v3 target = position + forward;
            return math::CreateLookAtMatrix(position, target, worldUp);
        }

        // Getters
        math::v3 GetPosition() const { return position; }
        math::v3 GetRotation() const { return rotation; }
        math::v3 GetForward() const { return forward; }
        math::v3 GetRight() const { return right; }
        math::v3 GetUp() const { return up; }

        // Setters
        void SetSpeed(float move, float rotate) {
            moveSpeed = move;
            rotateSpeed = rotate;
        }

    private:
        // Camera Attributes
        math::v3 position{ 0.0f, 0.0f, 0.0f };
        math::v3 rotation{ 0.0f, 0.0f, 0.0f }; // x=Pitch, y=Yaw, z=Roll
        
        // Camera Vectors
        math::v3 forward{ 0.0f, 0.0f, -1.0f };
        math::v3 right{ 1.0f, 0.0f, 0.0f };
        math::v3 up{ 0.0f, 1.0f, 0.0f };
        const math::v3 worldUp{ 0.0f, 1.0f, 0.0f };

        // Control Settings
        float moveSpeed{ 10.0f };
        float rotateSpeed{ 0.1f };

        /**
         * @brief 处理输入
         */
        void HandleInput(float dt) {
            using namespace primal::input;

            // 1. Mouse Rotation (Right Mouse Button Hold)
            input_value mouseRight;
            get(input_source::mouse, input_code::mouse_rigth, mouseRight); // Note: typo in Input.h 'mouse_rigth'

            if (mouseRight.current.x > 0.5f) { // Button Pressed
                input_value mousePos;
                get(input_source::mouse, input_code::mouse_position, mousePos);
                
                float dx = mousePos.current.x - mousePos.previous.x;
                float dy = mousePos.current.y - mousePos.previous.y;

                rotation.y += dx * rotateSpeed; // Yaw
                rotation.x += dy * rotateSpeed; // Pitch

                // Constrain Pitch
                if (rotation.x > 89.0f) rotation.x = 89.0f;
                if (rotation.x < -89.0f) rotation.x = -89.0f;

                UpdateVectors();
            }

            // 2. Keyboard Movement
            input_value val;
            math::v3 velocity{ 0.0f, 0.0f, 0.0f };

            // Forward/Backward
            get(input_source::keyboard, input_code::key_w, val);
            if (val.current.x > 0.0f) velocity = velocity + forward;
            get(input_source::keyboard, input_code::key_s, val);
            if (val.current.x > 0.0f) velocity = velocity - forward;

            // Left/Right
            get(input_source::keyboard, input_code::key_d, val);
            if (val.current.x > 0.0f) velocity = velocity + right;
            get(input_source::keyboard, input_code::key_a, val);
            if (val.current.x > 0.0f) velocity = velocity - right;

            // Up/Down (Q/E)
            get(input_source::keyboard, input_code::key_e, val);
            if (val.current.x > 0.0f) velocity = velocity + worldUp;
            get(input_source::keyboard, input_code::key_q, val);
            if (val.current.x > 0.0f) velocity = velocity - worldUp;

            // Apply Movement
            if (math::Length(velocity) > 0.0f) {
                velocity = math::Normalize(velocity);
                position = position + (velocity * moveSpeed * dt);
            }
        }

        /**
         * @brief 更新方向向量
         */
        void UpdateVectors() {
            // Calculate new forward vector
            math::v3 newForward;
            float pitchRad = rotation.x * math::constants::DEG_TO_RAD;
            float yawRad = rotation.y * math::constants::DEG_TO_RAD;

            // RHS System: Forward is -Z usually, but depends on Engine convention.
            // Assuming standard OpenGL/Metal: 
            // x = cos(yaw)cos(pitch)
            // y = sin(pitch)
            // z = sin(yaw)cos(pitch)
            // But we need to check the coordinate system.
            // Sponza Test used: Forward = Normalize(Center - Eye). 
            // If we assume Y-Up, then:
            
            newForward.x = cos(yawRad) * cos(pitchRad);
            newForward.y = sin(pitchRad);
            newForward.z = sin(yawRad) * cos(pitchRad);

            forward = math::Normalize(newForward);
            right = math::Normalize(math::Cross(forward, worldUp));
            up = math::Normalize(math::Cross(right, forward));
        }
    };

} // namespace primal::graphics::rhi
