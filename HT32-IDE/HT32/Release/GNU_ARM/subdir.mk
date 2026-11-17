################################################################################
# 自动生成的文件。不要编辑！
################################################################################

# 将这些工具调用的输入和输出添加到构建变量 
S_UPPER_SRCS += \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/project/HT32-IDE/startup_ht32l5xxxx_gcc_01.S 

OBJS += \
./GNU_ARM/startup_ht32l5xxxx_gcc_01.o 

S_UPPER_DEPS += \
./GNU_ARM/startup_ht32l5xxxx_gcc_01.d 


# 每个子目录必须为构建它所贡献的源提供规则
GNU_ARM/startup_ht32l5xxxx_gcc_01.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/project/HT32-IDE/startup_ht32l5xxxx_gcc_01.S GNU_ARM/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross Assembler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -x assembler-with-cpp -DUSE_HT32_CHIP=37 -DUSE_LIBCFG_RAM_SIZE=0x1000 -Wa,-adhlns="$@.lst" -MD -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '


