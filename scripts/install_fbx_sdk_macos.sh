#!/bin/bash

# FBX SDK 自动安装脚本
# 用于在 macOS 上自动下载和安装 FBX SDK

set -e  # 遇到错误时退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置变量
FBX_SDK_VERSION="2020.3.7"
FBX_INSTALL_DIR="/Applications/Autodesk/FBX SDK"
FBX_TARGET_DIR="${FBX_INSTALL_DIR}/${FBX_SDK_VERSION}"
TEMP_DIR="/tmp/fbx-sdk-install"

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查是否已经安装
check_existing_installation() {
    log_info "检查现有 FBX SDK 安装..."
    
    if [ -f "${FBX_TARGET_DIR}/include/fbxsdk.h" ]; then
        log_success "FBX SDK 已安装在: ${FBX_TARGET_DIR}"
        return 0
    fi
    
    # 检查其他可能的版本
    for dir in "${FBX_INSTALL_DIR}"/*; do
        if [ -d "$dir" ] && [ -f "$dir/include/fbxsdk.h" ]; then
            log_warning "发现其他版本的 FBX SDK: $dir"
            log_info "当前配置版本: ${FBX_SDK_VERSION}"
            break
        fi
    done
    
    return 1
}

# 创建临时目录
create_temp_dir() {
    log_info "创建临时工作目录..."
    rm -rf "${TEMP_DIR}"
    mkdir -p "${TEMP_DIR}"
}

# 清理临时目录
cleanup_temp_dir() {
    log_info "清理临时文件..."
    rm -rf "${TEMP_DIR}"
}

# 检查 Homebrew 是否可用
check_homebrew() {
    if command -v brew &> /dev/null; then
        return 0
    else
        return 1
    fi
}

# 使用 Homebrew 安装
install_with_homebrew() {
    log_info "尝试使用 Homebrew 安装 FBX SDK..."
    
    if ! check_homebrew; then
        log_warning "Homebrew 未安装，跳过此方法"
        return 1
    fi
    
    # 检查是否有 fbx-sdk cask
    if brew list --cask | grep -q "fbx-sdk"; then
        log_success "FBX SDK 已通过 Homebrew 安装"
        
        # 找到实际安装位置
        HOMEBREW_FBX_DIR=$(brew --prefix fbx-sdk 2>/dev/null || echo "")
        if [ -n "$HOMEBREW_FBX_DIR" ] && [ -f "$HOMEBREW_FBX_DIR/include/fbxsdk.h" ]; then
            # 创建符号链接到标准位置
            mkdir -p "${FBX_INSTALL_DIR}"
            ln -sf "$HOMEBREW_FBX_DIR" "${FBX_TARGET_DIR}"
            log_success "创建符号链接: ${HOMEBREW_FBX_DIR} -> ${FBX_TARGET_DIR}"
            return 0
        fi
    fi
    
    # 尝试通过 Homebrew 安装
    log_info "正在通过 Homebrew 安装 FBX SDK..."
    if brew install --cask fbx-sdk; then
        log_success "FBX SDK 安装成功"
        
        # 创建符号链接
        HOMEBREW_FBX_DIR=$(brew --prefix fbx-sdk 2>/dev/null || echo "")
        if [ -n "$HOMEBREW_FBX_DIR" ]; then
            mkdir -p "${FBX_INSTALL_DIR}"
            ln -sf "$HOMEBREW_FBX_DIR" "${FBX_TARGET_DIR}"
            log_success "创建符号链接: ${HOMEBREW_FBX_DIR} -> ${FBX_TARGET_DIR}"
            return 0
        fi
    else
        log_warning "Homebrew 安装失败，将尝试手动安装"
    fi
    
    return 1
}

# 手动下载安装
install_manually() {
    log_info "需要手动下载和安装 FBX SDK"
    log_warning "由于 Autodesk 需要账户登录，无法完全自动化安装"
    
    echo ""
    log_info "请按照以下步骤手动安装："
    echo "1. 访问 Autodesk Developer Network:"
    echo "   https://aps.autodesk.com/developer/overview/fbx-sdk"
    echo ""
    echo "2. 登录您的 Autodesk 账户（或创建新账户）"
    echo ""
    echo "3. 点击 'Download SDK for mac'"
    echo "   选择 'FBX SDK ${FBX_SDK_VERSION}' 或最新版本"
    echo "   下载 'Clang (Universal Binary)' 版本"
    echo ""
    echo "4. 下载完成后，运行以下命令进行安装："
    echo "   # 假设下载文件为 ${TEMP_DIR}/fbxsdk.tar.gz"
    echo "   cd ${TEMP_DIR}"
    echo "   tar -xzf fbxsdk.tar.gz"
    echo "   sudo mkdir -p \"${FBX_INSTALL_DIR}\""
    echo "   sudo mv fbxsdk \"${FBX_TARGET_DIR}\""
    echo ""
    
    # 询问用户是否已经下载了文件
    read -p "您是否已经下载了 FBX SDK 文件？(y/n): " -n 1 -r
    echo
    
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        log_info "请将下载的文件移动到 ${TEMP_DIR} 并重命名为 'fbxsdk.tar.gz'"
        read -p "完成后按回车键继续..."
        
        if [ -f "${TEMP_DIR}/fbxsdk.tar.gz" ]; then
            log_info "解压 FBX SDK..."
            cd "${TEMP_DIR}"
            tar -xzf fbxsdk.tar.gz
            
            # 查找解压后的目录
            EXTRACTED_DIR=$(find . -maxdepth 1 -type d -name "fbxsdk*" | head -n 1 | sed 's^\./^^')
            
            if [ -n "$EXTRACTED_DIR" ] && [ -d "$EXTRACTED_DIR" ]; then
                log_info "安装 FBX SDK 到: ${FBX_TARGET_DIR}"
                sudo mkdir -p "${FBX_INSTALL_DIR}"
                sudo mv "$EXTRACTED_DIR" "${FBX_TARGET_DIR}"
                
                if [ -f "${FBX_TARGET_DIR}/include/fbxsdk.h" ]; then
                    log_success "FBX SDK 安装成功！"
                    return 0
                else
                    log_error "安装验证失败"
                    return 1
                fi
            else
                log_error "无法找到解压后的 FBX SDK 目录"
                return 1
            fi
        else
            log_error "未找到下载的文件: ${TEMP_DIR}/fbxsdk.tar.gz"
            return 1
        fi
    else
        log_info "请按照上述说明手动安装 FBX SDK，然后重新运行此脚本进行验证"
        return 1
    fi
}

# 验证安装
verify_installation() {
    log_info "验证 FBX SDK 安装..."
    
    if [ -f "${FBX_TARGET_DIR}/include/fbxsdk.h" ]; then
        log_success "FBX SDK 头文件验证成功"
        
        # 检查库文件
        if [ -f "${FBX_TARGET_DIR}/lib/clang/debug/libfbxsdk.dylib" ] || \
           [ -f "${FBX_TARGET_DIR}/lib/clang/release/libfbxsdk.dylib" ] || \
           [ -f "${FBX_TARGET_DIR}/lib/libfbxsdk.dylib" ]; then
            log_success "FBX SDK 库文件验证成功"
            return 0
        else
            log_warning "FBX SDK 库文件未找到，但头文件存在"
            return 1
        fi
    else
        log_error "FBX SDK 头文件验证失败"
        return 1
    fi
}

# 设置权限
set_permissions() {
    log_info "设置 FBX SDK 目录权限..."
    
    if [ -d "${FBX_TARGET_DIR}" ]; then
        sudo chmod -R 755 "${FBX_TARGET_DIR}"
        log_success "权限设置完成"
    fi
}

# 主函数
main() {
    echo "========================================"
    echo "FBX SDK 自动安装脚本 for macOS"
    echo "目标版本: ${FBX_SDK_VERSION}"
    echo "目标目录: ${FBX_TARGET_DIR}"
    echo "========================================"
    echo ""
    
    # 检查是否已经安装
    if check_existing_installation; then
        verify_installation
        log_success "FBX SDK 已准备就绪！"
        echo ""
        log_info "现在可以重新运行 CMake 来配置项目："
        echo "   cd build"
        echo "   cmake .."
        echo ""
        exit 0
    fi
    
    # 创建临时目录
    create_temp_dir
    
    # 设置清理陷阱
    trap cleanup_temp_dir EXIT
    
    # 尝试使用 Homebrew 安装
    if install_with_homebrew; then
        log_success "通过 Homebrew 安装成功！"
    else
        # 手动安装
        if ! install_manually; then
            log_error "安装失败"
            exit 1
        fi
    fi
    
    # 设置权限
    set_permissions
    
    # 验证安装
    if verify_installation; then
        log_success "FBX SDK 安装完成！"
        echo ""
        log_info "现在可以重新运行 CMake 来配置项目："
        echo "   cd build"
        echo "   cmake .."
        echo ""
        log_info "CMake 将会自动检测已安装的 FBX SDK"
    else
        log_error "FBX SDK 安装验证失败"
        exit 1
    fi
}

# 运行主函数
main "$@"