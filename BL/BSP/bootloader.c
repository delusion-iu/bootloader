#include "bootloader.h"
#include <string.h>

/**
 * @brief 帧头定义(双字节同步头)
 */
#define BOOTLOADER_FRAME_HEADER_0 0x55U /* 帧头第一个字节 */
#define BOOTLOADER_FRAME_HEADER_1 0xAAU /* 帧头第二个字节 */

/**
 * @brief ACK应答载荷长度
 * @note 载荷包含：原指令+错误码+当前状态(3字节)
 */
#define BOOTLOADER_ACK_PAYLOAD_LEN 3U

/**
 * @brief END指令载荷长度
 * @note 载荷仅包含CRC校验值(2字节)
 */
#define BOOTLOADER_END_PAYLOAD_LEN 2U

/**
 * @brief SRAM基础地址(STM32F103默认)
 */
#define SRAM_BASE_ADDRESS 0x20000000UL

/**
 * @brief SRAM大小(20KB，适配F103C8T6)
 */
#define SRAM_SIZE_BYTES (20UL * 1024UL)

/**
 * @brief SRAM结束地址
 */
#define SRAM_END_ADDRESS (SRAM_BASE_ADDRESS + SRAM_SIZE_BYTES)

/**
 * @brief 应用程序最小向量表大小
 * @note 至少包含栈指针(4B)+复位地址(4B)
 */
#define APP_MIN_VECTOR_SIZE 8U

#define BOOTLOADER_START_PAYLOAD_LEN 8U
#define BOOTLOADER_APP_CONFIG_MAGIC 0x41505043UL

typedef struct
{
    uint32_t magic;
    uint32_t app_address;
} Bootloader_AppConfig_t;

/**
 * @brief Bootloader状态机枚举
 */
typedef enum
{
    BOOT_STATE_IDLE = 0,  /* 空闲态：初始/超时后状态 */
    BOOT_STATE_READY,     /* 就绪态：握手成功，等待START指令 */
    BOOT_STATE_RECEIVING, /* 接收态：正在接收应用程序数据 */
    BOOT_STATE_FINISHED   /* 完成态：数据接收完成且校验通过 */
} Bootloader_State_t;

/**
 * @brief 引导加载程序帧结构
 * @note 解析后的帧数据存储格式
 */
typedef struct
{
    uint8_t cmd;                                    /* 指令码(对应Bootloader_Command_t) */
    uint8_t seq;                                    /* 帧序号：用于防丢包/乱序 */
    uint16_t len;                                   /* 载荷长度 */
    uint8_t payload[BOOTLOADER_FRAME_MAX_DATA_LEN]; /* 载荷数据 */
} Bootloader_Frame_t;

/**
 * @brief 串口DMA接收缓冲区
 * @note 原始接收数据缓存
 */
static uint8_t bootloader_rx_dma_buffer[BOOTLOADER_RX_BUFFER_SIZE];

/**
 * @brief 帧解析缓冲区
 * @note 拷贝后的帧数据，避免DMA覆盖
 */
static uint8_t bootloader_rx_frame_buffer[BOOTLOADER_RX_BUFFER_SIZE];

/**
 * @brief 接收帧长度
 * @note 由串口中断回调更新
 */
static volatile uint16_t bootloader_rx_frame_len = 0U;

/**
 * @brief 接收帧就绪标志
 * @note 1: 有新帧待处理 0: 无新帧
 */
static volatile uint8_t bootloader_rx_frame_ready = 0U;

/**
 * @brief 接收溢出标志
 * @note 帧长度超过缓冲区大小时置位
 */
static volatile uint8_t bootloader_rx_overflow = 0U;

/**
 * @brief Bootloader当前状态
 */
static Bootloader_State_t boot_state = BOOT_STATE_IDLE;

/**
 * @brief 启动计时戳
 * @note 用于计算握手超时
 */
static uint32_t boot_start_tick = 0U;

/**
 * @brief 跳转请求标志
 * @note 1: 触发跳转到应用程序
 */
static uint8_t jump_requested = 0U;

/**
 * @brief 启动时应用程序有效性标志
 * @note 初始化时检测应用程序是否有效
 */
static uint8_t app_valid_at_boot = 0U;

/**
 * @brief 已编程数据长度
 * @note 累计已写入FLASH的字节数
 */
static uint32_t programmed_length = 0U;

/**
 * @brief 预期总数据长度
 * @note 由START指令告知的镜像总长度
 */
static uint32_t expected_length = 0U;

/**
 * @brief 预期CRC值
 * @note 由END指令告知的镜像CRC
 */
static uint16_t expected_crc = 0U;

/**
 * @brief 计算得到的CRC值
 * @note 累计接收数据的CRC
 */
static uint16_t calculated_crc = 0U;

/**
 * @brief 待拼接字节
 * @note 处理奇数字节时的临时存储
 */
static uint8_t pending_byte = 0xFFU;

/**
 * @brief 待拼接字节有效标志
 * @note 1: pending_byte中有未处理的字节
 */
static uint8_t pending_byte_valid = 0U;

/**
 * @brief 预期帧序号
 * @note 用于校验DATA帧的顺序
 */
static uint8_t expected_seq = 0U;

/**
 * @brief 当前操作的FLASH页地址
 * @note 用于避免重复擦除同一页
 */
static uint32_t current_page = 0xFFFFFFFFUL;
static uint32_t active_app_address = APP_DEFAULT_ADDRESS;
static uint32_t session_app_address = APP_DEFAULT_ADDRESS;

/* 函数前置声明 */
static uint16_t Bootloader_Crc16(const uint8_t *data, uint16_t len);
static uint16_t Bootloader_Crc16Update(uint16_t seed, const uint8_t *data, uint16_t len);
static uint32_t Bootloader_GetConfiguredAppAddress(void);
static uint8_t Bootloader_IsAppAddressAllowed(uint32_t address);
static uint8_t Bootloader_IsAppRangeValid(uint32_t app_address, uint32_t offset, uint32_t len);
static uint8_t Bootloader_IsAppValidAtAddress(uint32_t app_address);
static HAL_StatusTypeDef Bootloader_SaveAppAddress(uint32_t app_address);
static void Bootloader_StartReceive(void);
static void Bootloader_ResetSession(uint8_t keep_ready_state);
static void Bootloader_SendFrame(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len);
static void Bootloader_SendAck(uint8_t seq, uint8_t cmd, Bootloader_Error_t err);
static void Bootloader_SendNack(uint8_t seq, uint8_t cmd, Bootloader_Error_t err);
static void Bootloader_ProcessRxFrame(void);
static Bootloader_Error_t Bootloader_ParseFrame(const uint8_t *buffer, uint16_t len, Bootloader_Frame_t *frame);
static void Bootloader_HandleFrame(const Bootloader_Frame_t *frame);
static Bootloader_Error_t Bootloader_HandleHello(const Bootloader_Frame_t *frame);
static Bootloader_Error_t Bootloader_HandleStart(const Bootloader_Frame_t *frame);
static Bootloader_Error_t Bootloader_HandleData(const Bootloader_Frame_t *frame);
static Bootloader_Error_t Bootloader_HandleEnd(const Bootloader_Frame_t *frame);
static Bootloader_Error_t Bootloader_HandleJump(const Bootloader_Frame_t *frame);
static uint8_t Bootloader_IsRangeValid(uint32_t offset, uint32_t len);
static HAL_StatusTypeDef Bootloader_ErasePageIfNeeded(uint32_t address);
static HAL_StatusTypeDef Bootloader_WriteHalfword(uint32_t address, uint16_t halfword);
static HAL_StatusTypeDef Bootloader_WriteData(const uint8_t *data, uint16_t len);
static HAL_StatusTypeDef Bootloader_FinalizeImage(void);
static void Bootloader_DeInitForJump(void);

/**
 * @brief 计算16位CRC(Modbus算法)
 * @param data 待校验数据指针
 * @param len 待校验数据长度
 * @retval 计算得到的CRC值
 */
static uint16_t Bootloader_Crc16(const uint8_t *data, uint16_t len)
{
    return Bootloader_Crc16Update(0xFFFFU, data, len);
}

/**
 * @brief 增量更新CRC值
 * @param seed 初始CRC值(上一次计算结果)
 * @param data 待校验数据指针
 * @param len 待校验数据长度
 * @retval 更新后的CRC值
 * @note 采用Modbus CRC16算法：多项式0xA001，初始值0xFFFF
 */
static uint16_t Bootloader_Crc16Update(uint16_t seed, const uint8_t *data, uint16_t len)
{
    uint16_t crc = seed;
    uint16_t i;
    uint8_t bit;

    for (i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8U; ++bit)
        {
            if ((crc & 0x0001U) != 0U)
            {
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            }
            else
            {
                crc >>= 1U;
            }
        }
    }

    return crc;
}

static uint8_t Bootloader_IsAppAddressAllowed(uint32_t address)
{
    if ((address < APP_DEFAULT_ADDRESS) || (address >= APP_FLASH_END_ADDRESS))
    {
        return 0U;
    }

    if ((address % FLASH_PAGE_SIZE) != 0U)
    {
        return 0U;
    }

    return 1U;
}

static uint8_t Bootloader_IsAppRangeValid(uint32_t app_address, uint32_t offset, uint32_t len)
{
    uint32_t start;
    uint32_t end;

    if ((Bootloader_IsAppAddressAllowed(app_address) == 0U) || (len == 0U))
    {
        return 0U;
    }

    start = app_address + offset;
    end = start + len;

    if (start < app_address)
    {
        return 0U;
    }

    if ((end < start) || (end > APP_FLASH_END_ADDRESS))
    {
        return 0U;
    }

    return 1U;
}

static uint8_t Bootloader_IsAppValidAtAddress(uint32_t app_address)
{
    uint32_t app_stack;
    uint32_t app_reset;

    if (Bootloader_IsAppAddressAllowed(app_address) == 0U)
    {
        return 0U;
    }

    app_stack = *(volatile uint32_t *)app_address;
    app_reset = *(volatile uint32_t *)(app_address + 4UL);

    if ((app_stack < SRAM_BASE_ADDRESS) || (app_stack > SRAM_END_ADDRESS))
    {
        return 0U;
    }

    if ((app_reset < app_address) || (app_reset >= APP_FLASH_END_ADDRESS))
    {
        return 0U;
    }

    if ((app_reset & 0x1U) == 0U)
    {
        return 0U;
    }

    return 1U;
}

static uint32_t Bootloader_GetConfiguredAppAddress(void)
{
    const Bootloader_AppConfig_t *config = (const Bootloader_AppConfig_t *)APP_CONFIG_PAGE_ADDRESS;

    if ((config->magic == BOOTLOADER_APP_CONFIG_MAGIC) &&
        (Bootloader_IsAppAddressAllowed(config->app_address) != 0U))
    {
        return config->app_address;
    }

    return APP_DEFAULT_ADDRESS;
}

/**
 * @brief 启动串口接收
 * @note 清除串口错误标志，启动IDLE中断+DMA接收
 */
static void Bootloader_StartReceive(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart1);  /* 清除溢出错误标志 */
    __HAL_UART_CLEAR_IDLEFLAG(&huart1); /* 清除IDLE中断标志 */
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, bootloader_rx_dma_buffer, BOOTLOADER_RX_BUFFER_SIZE);
}

/**
 * @brief 重置会话状态
 * @param keep_ready_state 是否保持就绪态
 *        1: 重置后进入READY态 0: 重置后进入IDLE态
 * @note 清空长度、CRC、序号等会话相关变量
 */
static void Bootloader_ResetSession(uint8_t keep_ready_state)
{
    programmed_length = 0U;
    expected_length = 0U;
    expected_crc = 0U;
    calculated_crc = 0xFFFFU;
    pending_byte = 0xFFU;
    pending_byte_valid = 0U;
    expected_seq = 0U;
    current_page = 0xFFFFFFFFUL;
    session_app_address = active_app_address;
    boot_state = keep_ready_state ? BOOT_STATE_READY : BOOT_STATE_IDLE;
}

/**
 * @brief 初始化引导加载程序
 * @note 初始化状态变量、检测应用程序有效性、启动串口接收
 */
void Bootloader_Init(void)
{
    active_app_address = Bootloader_GetConfiguredAppAddress();
    session_app_address = active_app_address;
    boot_start_tick = HAL_GetTick();             /* 记录启动时间 */
    app_valid_at_boot = Bootloader_IsAppValid(); /* 检测应用程序是否有效 */
    jump_requested = 0U;                         /* 清空跳转请求 */
    bootloader_rx_frame_ready = 0U;              /* 清空帧就绪标志 */
    bootloader_rx_frame_len = 0U;                /* 清空帧长度 */
    bootloader_rx_overflow = 0U;                 /* 清空溢出标志 */
    Bootloader_ResetSession(0U);                 /* 重置会话为IDLE态 */
    Bootloader_StartReceive();                   /* 启动串口接收 */
}

/**
 * @brief Bootloader主任务处理函数
 * @note 需在主循环中周期性调用，处理：
 *       1. 接收溢出异常
 *       2. 新帧解析与处理
 *       3. 跳转请求执行
 *       4. 握手超时自动跳转
 */
void Bootloader_Task(void)
{
    /* 处理接收溢出：发送NACK并重置 */
    if (bootloader_rx_overflow != 0U)
    {
        bootloader_rx_overflow = 0U;
        Bootloader_SendNack(0U, 0U, BOOTLOADER_ERR_BAD_LENGTH);
    }

    /* 处理新接收的帧 */
    if (bootloader_rx_frame_ready != 0U)
    {
        Bootloader_ProcessRxFrame();
    }

    /* 执行跳转请求：应用程序有效时跳转到APP */
    if ((jump_requested != 0U) && (Bootloader_IsAppValid() != 0U))
    {
        /* 延时保证串口数据发送完成 */
        HAL_Delay(10U);
        Bootloader_JumpToApp();
    }

    /* 握手超时处理：空闲/就绪态下超时则触发跳转 */
    if ((Bootloader_IsAppValid() != 0U) &&
        ((boot_state == BOOT_STATE_IDLE) || (boot_state == BOOT_STATE_READY)) &&
        ((HAL_GetTick() - boot_start_tick) >= BOOTLOADER_HANDSHAKE_TIMEOUT_MS))
    {
        jump_requested = 1U;
    }
}

/**
 * @brief 校验应用程序有效性
 * @retval 1: 有效 0: 无效
 * @note 校验规则：
 *       1. 栈指针在SRAM范围内
 *       2. 复位地址在APP FLASH范围内
 *       3. 复位地址是Thumb指令(最低位为1)
 */
uint8_t Bootloader_IsAppValid(void)
{
    return Bootloader_IsAppValidAtAddress(active_app_address);
}

/**
 * @brief 跳转到应用程序执行
 * @note 执行流程：
 *       1. 校验应用程序有效性
 *       2. 关闭Bootloader外设/中断
 *       3. 重映射中断向量表到APP地址
 *       4. 切换栈指针并执行复位函数
 */
void Bootloader_JumpToApp(void)
{
    uint32_t app_stack = *(volatile uint32_t *)active_app_address;         /* 读取APP栈指针 */
    uint32_t app_reset = *(volatile uint32_t *)(active_app_address + 4UL); /* 读取APP复位地址 */
    void (*app_entry)(void) = (void (*)(void))app_reset;                   /* 转换为函数指针 */

    /* 应用程序无效则返回 */
    if (Bootloader_IsAppValid() == 0U)
    {
        return;
    }

    /* 关闭Bootloader相关外设 */
    Bootloader_DeInitForJump();

    /* 重映射中断向量表到APP起始地址 */
    SCB->VTOR = active_app_address;
    __DSB(); /* 数据同步屏障：确保VTOR更新完成 */
    __ISB(); /* 指令同步屏障：刷新指令流水线 */

    /* 切换栈指针到APP栈 */
    __set_MSP(app_stack);
    __enable_irq(); /* 开启全局中断 */
    __DSB();
    __ISB();

    /* 执行APP复位函数 */
    app_entry();
}

/**
 * @brief 处理接收的帧数据
 * @note 1. 临界区拷贝帧数据(避免DMA覆盖)
 *       2. 解析帧结构
 *       3. 根据解析结果发送ACK/NACK
 */
static void Bootloader_ProcessRxFrame(void)
{
    Bootloader_Frame_t frame;
    uint8_t raw_frame[BOOTLOADER_RX_BUFFER_SIZE];
    uint16_t len;
    Bootloader_Error_t parse_result;

    /* 临界区：禁止中断，拷贝帧数据 */
    __disable_irq();
    len = bootloader_rx_frame_len;
    memcpy(raw_frame, bootloader_rx_frame_buffer, len);
    bootloader_rx_frame_ready = 0U; /* 清空就绪标志 */
    __enable_irq();

    /* 解析帧结构 */
    parse_result = Bootloader_ParseFrame(raw_frame, len, &frame);
    if (parse_result != BOOTLOADER_ERR_NONE)
    {
        Bootloader_SendNack(0U, 0U, parse_result);
        return;
    }

    /* 处理解析后的帧 */
    Bootloader_HandleFrame(&frame);
}

/**
 * @brief 解析串口接收的原始帧数据
 * @param buffer 原始帧数据指针
 * @param len 原始帧数据长度
 * @param frame 解析后的数据存储指针
 * @retval 错误码(BOOTLOADER_ERR_NONE表示成功)
 * @note 帧格式：
 *       | 0x55 | 0xAA | cmd | seq | len_L | len_H | payload... | crc_L | crc_H |
 */
static Bootloader_Error_t Bootloader_ParseFrame(const uint8_t *buffer, uint16_t len, Bootloader_Frame_t *frame)
{
    uint16_t payload_len;
    uint16_t frame_crc;
    uint16_t calc_crc;

    /* 基础校验：长度、指针合法性 */
    if ((len < 8U) || (buffer == NULL) || (frame == NULL))
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 校验帧头 */
    if ((buffer[0] != BOOTLOADER_FRAME_HEADER_0) || (buffer[1] != BOOTLOADER_FRAME_HEADER_1))
    {
        return BOOTLOADER_ERR_BAD_FRAME;
    }

    /* 解析载荷长度(小端序) */
    payload_len = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8);
    if (payload_len > BOOTLOADER_FRAME_MAX_DATA_LEN)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 校验总长度：载荷长度 + 8字节固定头/尾 */
    if (len != (uint16_t)(payload_len + 8U))
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 校验CRC */
    frame_crc = (uint16_t)buffer[len - 2U] | ((uint16_t)buffer[len - 1U] << 8);
    calc_crc = Bootloader_Crc16(&buffer[2], (uint16_t)(len - 4U)); /* 计算cmd到payload的CRC */
    if (frame_crc != calc_crc)
    {
        return BOOTLOADER_ERR_BAD_CRC;
    }

    /* 填充帧结构 */
    frame->cmd = buffer[2];
    frame->seq = buffer[3];
    frame->len = payload_len;
    if (payload_len > 0U)
    {
        memcpy(frame->payload, &buffer[6], payload_len);
    }

    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 处理解析后的帧
 * @param frame 解析后的帧结构指针
 * @note 根据指令类型分发到对应处理函数，发送ACK/NACK响应
 */
static void Bootloader_HandleFrame(const Bootloader_Frame_t *frame)
{
    Bootloader_Error_t result = BOOTLOADER_ERR_BAD_STATE;

    /* 根据指令分发处理 */
    switch (frame->cmd)
    {
    case BOOTLOADER_CMD_HELLO:
        result = Bootloader_HandleHello(frame);
        break;

    case BOOTLOADER_CMD_START:
        result = Bootloader_HandleStart(frame);
        break;

    case BOOTLOADER_CMD_DATA:
        result = Bootloader_HandleData(frame);
        break;

    case BOOTLOADER_CMD_END:
        result = Bootloader_HandleEnd(frame);
        break;

    case BOOTLOADER_CMD_JUMP:
        result = Bootloader_HandleJump(frame);
        break;

    default:
        result = BOOTLOADER_ERR_BAD_FRAME;
        break;
    }

    /* 发送响应 */
    if (result == BOOTLOADER_ERR_NONE)
    {
        Bootloader_SendAck(frame->seq, frame->cmd, BOOTLOADER_ERR_NONE);
    }
    else
    {
        Bootloader_SendNack(frame->seq, frame->cmd, result);
    }
}

/**
 * @brief 处理HELLO指令
 * @param frame 帧结构指针
 * @retval 错误码
 * @note 重置会话状态，刷新超时计时，进入就绪态
 */
static Bootloader_Error_t Bootloader_HandleHello(const Bootloader_Frame_t *frame)
{
    /* HELLO指令无载荷 */
    if (frame->len != 0U)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 重置会话为就绪态 */
    Bootloader_ResetSession(1U);
    boot_start_tick = HAL_GetTick(); /* 刷新超时计时 */
    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 处理START指令
 * @param frame 帧结构指针
 * @retval 错误码
 * @note 1. 解析镜像总长度
 *       2. 校验长度合法性
 *       3. 重置会话并进入接收态
 */
static Bootloader_Error_t Bootloader_HandleStart(const Bootloader_Frame_t *frame)
{
    uint32_t image_length;
    uint32_t target_app_address;

    /* START指令载荷固定8字节(镜像长度 + 目标应用地址) */
    if (frame->len != BOOTLOADER_START_PAYLOAD_LEN)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 解析镜像长度(小端序) */
    image_length = (uint32_t)frame->payload[0] |
                   ((uint32_t)frame->payload[1] << 8) |
                   ((uint32_t)frame->payload[2] << 16) |
                   ((uint32_t)frame->payload[3] << 24);
    target_app_address = (uint32_t)frame->payload[4] |
                         ((uint32_t)frame->payload[5] << 8) |
                         ((uint32_t)frame->payload[6] << 16) |
                         ((uint32_t)frame->payload[7] << 24);

    /* 校验长度合法性：至少包含向量表，地址范围有效 */
    if ((image_length < APP_MIN_VECTOR_SIZE) ||
        (Bootloader_IsAppRangeValid(target_app_address, 0U, image_length) == 0U))
    {
        return BOOTLOADER_ERR_RANGE;
    }

    /* 重置会话并进入接收态 */
    Bootloader_ResetSession(1U);
    expected_length = image_length;
    session_app_address = target_app_address;
    expected_seq = (uint8_t)(frame->seq + 1U); /* 下一帧预期序号 */
    boot_state = BOOT_STATE_RECEIVING;
    jump_requested = 0U; /* 清空跳转请求 */
    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 处理DATA指令
 * @param frame 帧结构指针
 * @retval 错误码
 * @note 1. 校验状态/序号/地址范围
 *       2. 写入FLASH
 *       3. 增量更新CRC
 */
static Bootloader_Error_t Bootloader_HandleData(const Bootloader_Frame_t *frame)
{
    HAL_StatusTypeDef flash_result;

    /* 校验状态：仅接收态可处理DATA指令 */
    if (boot_state != BOOT_STATE_RECEIVING)
    {
        return BOOTLOADER_ERR_BAD_STATE;
    }

    /* 校验序号 */
    if (frame->seq != expected_seq)
    {
        return BOOTLOADER_ERR_BAD_SEQUENCE;
    }

    /* 校验载荷长度和地址范围 */
    if ((frame->len == 0U) || (Bootloader_IsRangeValid(programmed_length, frame->len) == 0U))
    {
        return BOOTLOADER_ERR_RANGE;
    }

    /* 解锁FLASH并写入数据 */
    HAL_FLASH_Unlock();
    flash_result = Bootloader_WriteData(frame->payload, frame->len);
    HAL_FLASH_Lock();

    /* 校验FLASH写入结果 */
    if (flash_result != HAL_OK)
    {
        return BOOTLOADER_ERR_FLASH;
    }

    /* 增量更新CRC */
    calculated_crc = Bootloader_Crc16Update(calculated_crc, frame->payload, frame->len);
    expected_seq++; /* 更新预期序号 */
    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 处理END指令
 * @param frame 帧结构指针
 * @retval 错误码
 * @note 1. 校验状态/载荷长度/已编程长度
 *       2. 完成最后一字节写入
 *       3. 校验CRC和应用程序有效性
 */
static Bootloader_Error_t Bootloader_HandleEnd(const Bootloader_Frame_t *frame)
{
    HAL_StatusTypeDef flash_result;
    HAL_StatusTypeDef config_result;

    /* 校验状态：仅接收态可处理END指令 */
    if (boot_state != BOOT_STATE_RECEIVING)
    {
        return BOOTLOADER_ERR_BAD_STATE;
    }

    /* 校验载荷长度：固定2字节(CRC值) */
    if (frame->len != BOOTLOADER_END_PAYLOAD_LEN)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 解析预期CRC */
    expected_crc = (uint16_t)frame->payload[0] | ((uint16_t)frame->payload[1] << 8);

    /* 校验已编程长度是否匹配预期 */
    if (programmed_length != expected_length)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 解锁FLASH并完成最后数据写入 */
    HAL_FLASH_Unlock();
    flash_result = Bootloader_FinalizeImage();
    HAL_FLASH_Lock();
    if (flash_result != HAL_OK)
    {
        return BOOTLOADER_ERR_FLASH;
    }

    /* 校验CRC */
    if (expected_crc != calculated_crc)
    {
        return BOOTLOADER_ERR_VERIFY;
    }

    /* 校验应用程序有效性 */
    if (Bootloader_IsAppValidAtAddress(session_app_address) == 0U)
    {
        return BOOTLOADER_ERR_APP_INVALID;
    }

    HAL_FLASH_Unlock();
    config_result = Bootloader_SaveAppAddress(session_app_address);
    HAL_FLASH_Lock();
    if (config_result != HAL_OK)
    {
        return BOOTLOADER_ERR_FLASH;
    }

    active_app_address = session_app_address;

    /* 进入完成态 */
    boot_state = BOOT_STATE_FINISHED;
    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 处理JUMP指令
 * @param frame 帧结构指针
 * @retval 错误码
 * @note 触发跳转请求，校验应用程序有效性
 */
static Bootloader_Error_t Bootloader_HandleJump(const Bootloader_Frame_t *frame)
{
    /* JUMP指令无载荷 */
    if (frame->len != 0U)
    {
        return BOOTLOADER_ERR_BAD_LENGTH;
    }

    /* 校验应用程序有效性 */
    if (Bootloader_IsAppValid() == 0U)
    {
        return BOOTLOADER_ERR_APP_INVALID;
    }

    /* 触发跳转请求 */
    jump_requested = 1U;
    return BOOTLOADER_ERR_NONE;
}

/**
 * @brief 校验地址范围合法性
 * @param offset 相对于APP_ADDRESS的偏移
 * @param len 数据长度
 * @retval 1: 合法 0: 非法
 * @note 确保地址范围在APP FLASH区间内，无溢出
 */
static uint8_t Bootloader_IsRangeValid(uint32_t offset, uint32_t len)
{
    return Bootloader_IsAppRangeValid(session_app_address, offset, len);
}

/**
 * @brief 按需擦除FLASH页
 * @param address 要写入的地址
 * @retval HAL状态码
 * @note 同一页仅擦除一次，避免重复擦除
 */
static HAL_StatusTypeDef Bootloader_ErasePageIfNeeded(uint32_t address)
{
    uint32_t page_start = address - (address % FLASH_PAGE_SIZE); /* 计算页起始地址 */
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;

    /* 同一页无需重复擦除 */
    if (current_page == page_start)
    {
        return HAL_OK;
    }

    /* 配置擦除参数 */
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = page_start;
    erase.NbPages = 1U;

    /* 执行擦除 */
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
    {
        return HAL_ERROR;
    }

    /* 更新当前页地址 */
    current_page = page_start;
    return HAL_OK;
}

/**
 * @brief 写入半字(16位)到FLASH
 * @param address 写入地址(必须半字对齐)
 * @param halfword 要写入的半字数据
 * @retval HAL状态码
 * @note 1. 先擦除对应页(按需)
 *       2. 写入后校验数据
 */
static HAL_StatusTypeDef Bootloader_WriteHalfword(uint32_t address, uint16_t halfword)
{
    /* 按需擦除页 */
    if (Bootloader_ErasePageIfNeeded(address) != HAL_OK)
    {
        return HAL_ERROR;
    }

    /* 写入半字 */
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address, halfword) != HAL_OK)
    {
        return HAL_ERROR;
    }

    /* 校验写入结果 */
    if (*(volatile uint16_t *)address != halfword)
    {
        return HAL_ERROR;
    }

    return HAL_OK;
}

/**
 * @brief 写入数据到FLASH
 * @param data 数据指针
 * @param len 数据长度
 * @retval HAL状态码
 * @note 1. 按半字拼接数据(处理奇数字节)
 *       2. 累计已编程长度
 */
static HAL_StatusTypeDef Bootloader_WriteData(const uint8_t *data, uint16_t len)
{
    uint16_t index;
    uint16_t halfword;
    uint32_t write_address;

    /* 校验参数 */
    if ((data == NULL) || (len == 0U))
    {
        return HAL_ERROR;
    }

    /* 逐字节处理 */
    for (index = 0U; index < len; ++index)
    {
        if (pending_byte_valid == 0U)
        {
            /* 奇数字节：暂存待拼接 */
            pending_byte = data[index];
            pending_byte_valid = 1U;
        }
        else
        {
            /* 偶数字节：拼接为半字并写入 */
            halfword = (uint16_t)pending_byte | ((uint16_t)data[index] << 8);
            write_address = session_app_address + programmed_length - 1U; /* 计算写入地址 */
            if (Bootloader_WriteHalfword(write_address, halfword) != HAL_OK)
            {
                return HAL_ERROR;
            }
            pending_byte_valid = 0U; /* 清空待拼接标志 */
        }

        programmed_length++; /* 累计已编程长度 */
    }

    return HAL_OK;
}

/**
 * @brief 完成镜像写入(处理最后一个奇数字节)
 * @retval HAL状态码
 * @note 若存在未拼接的字节，补0xFF后写入
 */
static HAL_StatusTypeDef Bootloader_FinalizeImage(void)
{
    /* 处理最后一个奇数字节 */
    if (pending_byte_valid != 0U)
    {
        if (Bootloader_WriteHalfword(session_app_address + programmed_length - 1U, (uint16_t)pending_byte | 0xFF00U) != HAL_OK)
        {
            return HAL_ERROR;
        }
        pending_byte_valid = 0U;
    }

    return HAL_OK;
}

static HAL_StatusTypeDef Bootloader_SaveAppAddress(uint32_t app_address)
{
    Bootloader_AppConfig_t config;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;
    uint16_t *halfwords = (uint16_t *)&config;
    uint32_t address = APP_CONFIG_PAGE_ADDRESS;
    uint32_t index;

    if (Bootloader_IsAppAddressAllowed(app_address) == 0U)
    {
        return HAL_ERROR;
    }

    config.magic = BOOTLOADER_APP_CONFIG_MAGIC;
    config.app_address = app_address;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_CONFIG_PAGE_ADDRESS;
    erase.NbPages = 1U;
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK)
    {
        return HAL_ERROR;
    }

    for (index = 0U; index < (sizeof(config) / sizeof(uint16_t)); ++index)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address, halfwords[index]) != HAL_OK)
        {
            return HAL_ERROR;
        }

        if (*(volatile uint16_t *)address != halfwords[index])
        {
            return HAL_ERROR;
        }

        address += sizeof(uint16_t);
    }

    return HAL_OK;
}

/**
 * @brief 发送帧数据到主机
 * @param cmd 指令码
 * @param seq 帧序号
 * @param payload 载荷数据指针
 * @param len 载荷长度
 * @note 按帧格式组装数据并通过串口发送
 */
static void Bootloader_SendFrame(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[BOOTLOADER_FRAME_MAX_DATA_LEN + 8U];
    uint16_t crc;

    /* 校验载荷长度 */
    if (len > BOOTLOADER_FRAME_MAX_DATA_LEN)
    {
        return;
    }

    /* 组装帧头 */
    frame[0] = BOOTLOADER_FRAME_HEADER_0;
    frame[1] = BOOTLOADER_FRAME_HEADER_1;
    frame[2] = cmd;
    frame[3] = seq;
    frame[4] = (uint8_t)(len & 0xFFU);
    frame[5] = (uint8_t)(len >> 8);

    /* 拷贝载荷 */
    if ((payload != NULL) && (len > 0U))
    {
        memcpy(&frame[6], payload, len);
    }

    /* 计算CRC并填充 */
    crc = Bootloader_Crc16(&frame[2], (uint16_t)(len + 4U));
    frame[6U + len] = (uint8_t)(crc & 0xFFU);
    frame[7U + len] = (uint8_t)(crc >> 8);

    /* 发送帧 */
    HAL_UART_Transmit_IT(&huart1, frame, (uint16_t)(len + 8U));
}

/**
 * @brief 发送ACK响应
 * @param seq 帧序号(对应请求帧的序号)
 * @param cmd 原指令码
 * @param err 错误码(BOOTLOADER_ERR_NONE表示成功)
 * @note 载荷格式：[原指令码][错误码][当前状态]
 */
static void Bootloader_SendAck(uint8_t seq, uint8_t cmd, Bootloader_Error_t err)
{
    uint8_t payload[BOOTLOADER_ACK_PAYLOAD_LEN];

    payload[0] = cmd;
    payload[1] = (uint8_t)err;
    payload[2] = (uint8_t)boot_state;
    Bootloader_SendFrame(BOOTLOADER_CMD_ACK, seq, payload, BOOTLOADER_ACK_PAYLOAD_LEN);
}

/**
 * @brief 发送NACK响应
 * @param seq 帧序号(对应请求帧的序号)
 * @param cmd 原指令码
 * @param err 错误码
 * @note 载荷格式与ACK相同，指令码为NACK
 */
static void Bootloader_SendNack(uint8_t seq, uint8_t cmd, Bootloader_Error_t err)
{
    uint8_t payload[BOOTLOADER_ACK_PAYLOAD_LEN];

    payload[0] = cmd;
    payload[1] = (uint8_t)err;
    payload[2] = (uint8_t)boot_state;
    Bootloader_SendFrame(BOOTLOADER_CMD_NACK, seq, payload, BOOTLOADER_ACK_PAYLOAD_LEN);
}

/**
 * @brief 跳转前反初始化Bootloader
 * @note 1. 关闭串口中断/外设
 *       2. 关闭SysTick
 *       3. 禁用所有中断
 *       4. 确保APP运行环境干净
 */
static void Bootloader_DeInitForJump(void)
{
    uint32_t index;

    /* 禁用串口1所有中断 */
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_RXNE);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_IDLE);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_TC);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_TXE);
    __HAL_UART_DISABLE_IT(&huart1, UART_IT_ERR);

    /* 关闭SysTick */
    HAL_SuspendTick();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    /* 终止串口接收并反初始化 */
    HAL_UART_AbortReceive(&huart1);
    HAL_UART_DeInit(&huart1);

    /* 禁用并清除串口1中断 */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART1_IRQn);

    /* 禁用所有中断并清除挂起标志 */
    __disable_irq();
    for (index = 0U; index < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); ++index)
    {
        NVIC->ICER[index] = 0xFFFFFFFFUL; /* 禁用所有中断 */
        NVIC->ICPR[index] = 0xFFFFFFFFUL; /* 清除所有挂起中断 */
    }
}

/**
 * @brief 串口接收事件回调函数
 * @param huart 串口句柄
 * @param size 接收数据长度
 * @note 1. 拷贝DMA接收数据到帧缓冲区
 *       2. 标记帧就绪或溢出
 *       3. 重启接收
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == USART1)
    {
        /* 数据未溢出则拷贝到帧缓冲区 */
        if ((bootloader_rx_frame_ready == 0U) && (size <= BOOTLOADER_RX_BUFFER_SIZE))
        {
            memcpy(bootloader_rx_frame_buffer, bootloader_rx_dma_buffer, size);
            bootloader_rx_frame_len = size;
            bootloader_rx_frame_ready = 1U;
        }
        else
        {
            /* 标记溢出 */
            bootloader_rx_overflow = 1U;
        }

        /* 重启接收 */
        Bootloader_StartReceive();
    }
}
