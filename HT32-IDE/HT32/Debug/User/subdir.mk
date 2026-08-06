################################################################################
# 自动生成的文件。不要编辑！
################################################################################

# 将这些工具调用的输入和输出添加到构建变量 
C_SRCS += \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/BFTM0.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/Display.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/LED.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/hd9803.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/ht32l5xxxx_01_it.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/main.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/spi_lcd.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/time_module.c \
C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/uart_init.c 

C_DEPS += \
./User/BFTM0.d \
./User/Display.d \
./User/LED.d \
./User/hd9803.d \
./User/ht32l5xxxx_01_it.d \
./User/main.d \
./User/spi_lcd.d \
./User/time_module.d \
./User/uart_init.d 

OBJS += \
./User/BFTM0.o \
./User/Display.o \
./User/LED.o \
./User/hd9803.o \
./User/ht32l5xxxx_01_it.o \
./User/main.o \
./User/spi_lcd.o \
./User/time_module.o \
./User/uart_init.o 


# 每个子目录必须为构建它所贡献的源提供规则
User/BFTM0.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/BFTM0.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/Display.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/Display.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/LED.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/LED.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/hd9803.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/hd9803.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/ht32l5xxxx_01_it.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/ht32l5xxxx_01_it.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/main.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/main.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/spi_lcd.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/spi_lcd.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/time_module.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/time_module.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

User/uart_init.o: C:/Users/q3112/Desktop/HT/4/HT32L52231_QFN32/project/uart_init.c User/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '


