################################################################################
# 自动生成的文件。不要编辑！
################################################################################

-include ../makefile.init

RM := rm -rf

# 此处定义了参与生成的所有源
-include sources.mk
-include Utilities/subdir.mk
-include User/subdir.mk
-include Retarget/subdir.mk
-include Library/subdir.mk
-include GNU_ARM/subdir.mk
-include Config/subdir.mk
-include CMSIS/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(CC_DEPS)),)
-include $(CC_DEPS)
endif
ifneq ($(strip $(SX_DEPS)),)
-include $(SX_DEPS)
endif
ifneq ($(strip $(C++_DEPS)),)
-include $(C++_DEPS)
endif
ifneq ($(strip $(CCM_DEPS)),)
-include $(CCM_DEPS)
endif
ifneq ($(strip $(C_UPPER_DEPS)),)
-include $(C_UPPER_DEPS)
endif
ifneq ($(strip $(CXX_DEPS)),)
-include $(CXX_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(CXXM_DEPS)),)
-include $(CXXM_DEPS)
endif
ifneq ($(strip $(C++M_DEPS)),)
-include $(C++M_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
ifneq ($(strip $(CPP_DEPS)),)
-include $(CPP_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := 0714_17313372_L52231
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# 将这些工具调用的输入和输出添加到构建变量 
SECONDARY_FLASH += \
0714_17313372_L52231.hex \

SECONDARY_SIZE += \
0714_17313372_L52231.siz \


# 所有目标
all: main-build

# 主构建目标
main-build: 0714_17313372_L52231.elf secondary-outputs

# 工具调用
0714_17313372_L52231.elf: $(OBJS) $(USER_OBJS) makefile objects.mk $(OPTIONAL_TOOL_DEPS)
	@echo '正在构建目标: $@'
	@echo '正在调用: GNU Arm Cross C++ Linker'
	arm-none-eabi-g++ -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -T ../../linker.ld -Xlinker --gc-sections -Xlinker --no-warn-rwx-segments -Wl,-Map,"0714_17313372_L52231.map" --specs=nano.specs --specs=nosys.specs -lm -Wl,--gc-sections -Wl,--print-memory-usage -o "0714_17313372_L52231.elf" $(OBJS) $(USER_OBJS) $(LIBS)
	@echo '已结束构建目标： $@'
	@echo ' '

0714_17313372_L52231.hex: 0714_17313372_L52231.elf makefile objects.mk $(OPTIONAL_TOOL_DEPS)
	@echo '正在调用: GNU Arm Cross Create Flash Image'
	arm-none-eabi-objcopy -O ihex "0714_17313372_L52231.elf"  "0714_17313372_L52231.hex"
	@echo '已结束构建： $@'
	@echo ' '

0714_17313372_L52231.siz: 0714_17313372_L52231.elf makefile objects.mk $(OPTIONAL_TOOL_DEPS)
	@echo '正在调用: GNU Arm Cross Print Size'
	arm-none-eabi-size --format=berkeley "0714_17313372_L52231.elf"
	@echo '已结束构建： $@'
	@echo ' '

# 其他目标
clean:
	-$(RM) $(CCM_DEPS)$(C_UPPER_DEPS)$(SECONDARY_SIZE)$(CXXM_DEPS)$(C_DEPS)$(CC_DEPS)$(SX_DEPS)$(C++_DEPS)$(OBJS)$(CXX_DEPS)$(SECONDARY_FLASH)$(ASM_DEPS)$(S_UPPER_DEPS)$(C++M_DEPS)$(CPP_DEPS) 0714_17313372_L52231.elf
	-@echo ' '

secondary-outputs: $(SECONDARY_FLASH) $(SECONDARY_SIZE)

.PHONY: all clean dependents main-build

-include ../makefile.targets
