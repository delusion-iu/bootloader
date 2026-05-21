#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "main.h"
#include "usart.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 串口接收缓冲区大小
 * @note 最大支持512字节的单帧数据接收
 */
#define BOOTLOADER_RX_BUFFER_SIZE 512U

/**
 * @brief 引导加载程序帧最大有效载荷长度
 * @note 单帧数据段最大256字节
 */
#define BOOTLOADER_FRAME_MAX_DATA_LEN 256U

/**
 * @brief 握手超时时间(毫秒)
 * @note 超时后自动跳转到应用程序，默认9秒
 */
#define BOOTLOADER_HANDSHAKE_TIMEOUT_MS 9000U

/**
 * @brief FLASH基础地址(STM32F103默认)
 */
#define FLASH_BASE_ADDRESS 0x08000000UL

/**
 * @brief FLASH总大小(64KB)
 */
#define FLASH_TOTAL_SIZE (64UL * 1024UL)

/**
 * @brief FLASH结束地址
 */
#define FLASH_END_ADDRESS (FLASH_BASE_ADDRESS + FLASH_TOTAL_SIZE)

/**
 * @brief 引导加载程序占用FLASH大小(16KB)
 */
#define BOOTLOADER_SIZE (16UL * 1024UL)

/**
 * @brief 应用程序起始地址(0x08004000，即16KB偏移处)
 */
#define APP_DEFAULT_ADDRESS 0x08004000UL

/**
 * @brief 应用程序FLASH结束地址(同FLASH总结束地址)
 */
#define APP_CONFIG_PAGE_ADDRESS (FLASH_END_ADDRESS - FLASH_PAGE_SIZE)
#define APP_FLASH_END_ADDRESS APP_CONFIG_PAGE_ADDRESS

    /**
     * @brief 引导加载程序命令枚举
     * @note 主机与Bootloader交互的指令集
     */
    typedef enum
    {
        BOOTLOADER_CMD_HELLO = 0x01, /* 握手指令：重置会话并准备接收 */
        BOOTLOADER_CMD_START = 0x02, /* 开始传输：告知待烧录镜像长度 */
        BOOTLOADER_CMD_DATA = 0x03,  /* 数据传输：发送应用程序数据段 */
        BOOTLOADER_CMD_END = 0x04,   /* 结束传输：校验CRC并完成烧录 */
        BOOTLOADER_CMD_JUMP = 0x05,  /* 跳转指令：执行已烧录的应用程序 */
        BOOTLOADER_CMD_ACK = 0x80,   /* 应答指令：正确处理帧后的响应 */
        BOOTLOADER_CMD_NACK = 0x81   /* 否定应答：处理帧失败后的响应 */
    } Bootloader_Command_t;

    /**
     * @brief 引导加载程序错误码枚举
     * @note 用于告知主机处理失败的原因
     */
    typedef enum
    {
        BOOTLOADER_ERR_NONE = 0x00,         /* 无错误 */
        BOOTLOADER_ERR_BAD_FRAME = 0x01,    /* 帧头错误 */
        BOOTLOADER_ERR_BAD_CRC = 0x02,      /* CRC校验失败 */
        BOOTLOADER_ERR_BAD_LENGTH = 0x03,   /* 帧长度非法 */
        BOOTLOADER_ERR_BAD_STATE = 0x04,    /* Bootloader状态不匹配 */
        BOOTLOADER_ERR_BAD_SEQUENCE = 0x05, /* 帧序号错误 */
        BOOTLOADER_ERR_RANGE = 0x06,        /* 地址范围越界 */
        BOOTLOADER_ERR_FLASH = 0x07,        /* FLASH擦写失败 */
        BOOTLOADER_ERR_VERIFY = 0x08,       /* 镜像校验失败(CRC不匹配) */
        BOOTLOADER_ERR_APP_INVALID = 0x09,  /* 应用程序无效(栈/复位地址非法) */
        BOOTLOADER_ERR_NOT_READY = 0x0A     /* Bootloader未就绪 */
    } Bootloader_Error_t;

    /**
     * @brief 初始化引导加载程序
     * @note 初始化接收缓冲区、状态机、超时计时，启动串口接收
     */
    void Bootloader_Init(void);

    /**
     * @brief 引导加载程序主任务
     * @note 需在主循环中调用，处理接收帧、超时跳转、异常处理
     */
    void Bootloader_Task(void);

    /**
     * @brief 检查应用程序是否有效
     * @retval 1: 有效 0: 无效
     * @note 校验栈地址、复位地址合法性
     */
    uint8_t Bootloader_IsAppValid(void);

    /**
     * @brief 跳转到应用程序执行
     * @note 关闭Bootloader外设、重映射中断向量表、切换栈指针后执行应用程序
     */
    void Bootloader_JumpToApp(void);

#ifdef __cplusplus
}
#endif

#endif
