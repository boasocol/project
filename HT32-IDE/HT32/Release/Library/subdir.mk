################################################################################
# 自动生成的文件。不要编辑！
################################################################################

# 将这些工具调用的输入和输出添加到构建变量 
C_SRCS += \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_cm0plus_misc.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_adc.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_bftm.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_ckcu.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_crc.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_exti.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_flash.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_gpio.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_i2c.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_mctm.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_pdma.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_pwrcu.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_rstcu.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_rtc.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_spi.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_tm.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_usart.c \
C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_wdt.c 

C_DEPS += \
./Library/ht32_cm0plus_misc.d \
./Library/ht32l5xxxx_adc.d \
./Library/ht32l5xxxx_bftm.d \
./Library/ht32l5xxxx_ckcu.d \
./Library/ht32l5xxxx_crc.d \
./Library/ht32l5xxxx_exti.d \
./Library/ht32l5xxxx_flash.d \
./Library/ht32l5xxxx_gpio.d \
./Library/ht32l5xxxx_i2c.d \
./Library/ht32l5xxxx_mctm.d \
./Library/ht32l5xxxx_pdma.d \
./Library/ht32l5xxxx_pwrcu.d \
./Library/ht32l5xxxx_rstcu.d \
./Library/ht32l5xxxx_rtc.d \
./Library/ht32l5xxxx_spi.d \
./Library/ht32l5xxxx_tm.d \
./Library/ht32l5xxxx_usart.d \
./Library/ht32l5xxxx_wdt.d 

OBJS += \
./Library/ht32_cm0plus_misc.o \
./Library/ht32l5xxxx_adc.o \
./Library/ht32l5xxxx_bftm.o \
./Library/ht32l5xxxx_ckcu.o \
./Library/ht32l5xxxx_crc.o \
./Library/ht32l5xxxx_exti.o \
./Library/ht32l5xxxx_flash.o \
./Library/ht32l5xxxx_gpio.o \
./Library/ht32l5xxxx_i2c.o \
./Library/ht32l5xxxx_mctm.o \
./Library/ht32l5xxxx_pdma.o \
./Library/ht32l5xxxx_pwrcu.o \
./Library/ht32l5xxxx_rstcu.o \
./Library/ht32l5xxxx_rtc.o \
./Library/ht32l5xxxx_spi.o \
./Library/ht32l5xxxx_tm.o \
./Library/ht32l5xxxx_usart.o \
./Library/ht32l5xxxx_wdt.o 


# 每个子目录必须为构建它所贡献的源提供规则
Library/ht32_cm0plus_misc.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32_cm0plus_misc.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_adc.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_adc.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_bftm.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_bftm.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_ckcu.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_ckcu.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_crc.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_crc.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_exti.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_exti.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_flash.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_flash.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_gpio.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_gpio.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_i2c.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_i2c.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_mctm.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_mctm.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_pdma.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_pdma.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_pwrcu.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_pwrcu.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_rstcu.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_rstcu.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_rtc.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_rtc.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_spi.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_spi.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_tm.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_tm.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_usart.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_usart.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '

Library/ht32l5xxxx_wdt.o: C:/Users/q3112/Desktop/HT/7/HT32L52231_QFN32/library/HT32L5xxxx_Driver/src/ht32l5xxxx_wdt.c Library/subdir.mk
	@echo '正在构建文件: $<'
	@echo '正在调用: GNU Arm Cross C Compiler'
	arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -mthumb-interwork -Os -ffunction-sections -fdata-sections -Wall -g -gdwarf-2 -DUSE_HT32_DRIVER -DUSE_HT32L52241_SK -DUSE_HT32L52231_41 -DUSE_MEM_HT32L52231 -I../../../ -I../../../app -I../../../driver -I../../../bsp -I../../../../library/CMSIS/Include -I../../../../library/Device/Holtek/HT32L5xxxx/Include -I../../../../library/HT32L5xxxx_Driver/inc -I../../../../utilities -std=gnu11 -Wa,-adhlns="$@.lst" -c -MD -mapcs-frame -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo '已结束构建： $<'
	@echo ' '


