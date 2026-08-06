################################################################################
# 自动生成的文件。不要编辑！
################################################################################

# 将这些工具调用的输入和输出添加到构建变量 
C_SRCS += \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_retarget.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_serial.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/printf.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/syscalls.c 

C_DEPS += \
./Retarget/ht32_retarget.d \
./Retarget/ht32_serial.d \
./Retarget/printf.d \
./Retarget/syscalls.d 

OBJS += \
./Retarget/ht32_retarget.o \
./Retarget/ht32_serial.o \
./Retarget/printf.o \
./Retarget/syscalls.o 


# 每个子目录必须为构建它所贡献的源提供规则
Retarget/ht32_retarget.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_retarget.c Retarget/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Retarget/ht32_serial.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_serial.c Retarget/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Retarget/printf.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/printf.c Retarget/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Retarget/syscalls.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/syscalls.c Retarget/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '


