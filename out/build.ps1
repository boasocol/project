# HT32L52231 firmware CLI build (mirrors HT32-IDE Debug/*/subdir.mk flags)
# Run:  Set-Location <project>; Get-Content out\build.ps1 -Raw | Invoke-Expression
$ErrorActionPreference = 'Continue'

$TC   = 'D:\HT32-IDE\xPack\arm-gnu-toolchain-13.2.Rel1-mingw-w64-i686-arm-none-eabi\bin'
$GCC  = Join-Path $TC 'arm-none-eabi-gcc.exe'
$SIZE = Join-Path $TC 'arm-none-eabi-size.exe'

$PROJ  = 'C:\Users\q3112\Desktop\HT\7\HT32L52231_QFN32\project'
$ROOT  = 'C:\Users\q3112\Desktop\HT\7\HT32L52231_QFN32'
$LIB   = Join-Path $ROOT 'library'
$UTIL  = Join-Path $ROOT 'utilities'
$OUT   = Join-Path $PROJ 'out'
$OBJD  = Join-Path $OUT 'obj'
$MAP   = Join-Path $OUT 'firmware.map'
$ELF   = Join-Path $OUT 'firmware.elf'

New-Item -ItemType Directory -Force -Path $OBJD | Out-Null
Get-ChildItem $OBJD -Include *.o,*.d,*.log -File -ErrorAction SilentlyContinue | Remove-Item -Force

$DEFS = @('-DUSE_HT32_DRIVER','-DUSE_HT32L52241_SK','-DUSE_HT32L52231_41','-DUSE_MEM_HT32L52231')
$INCS = @(
  "-I$PROJ",
  "-I$PROJ/app",
  "-I$PROJ/driver",
  "-I$PROJ/bsp",
  "-I$LIB/CMSIS/Include",
  "-I$LIB/Device/Holtek/HT32L5xxxx/Include",
  "-I$LIB/HT32L5xxxx_Driver/inc",
  "-I$UTIL"
)
$CFLAGS = @('-mcpu=cortex-m0plus','-mthumb','-mthumb-interwork','-Os','-ffunction-sections','-fdata-sections',
            '-Wall','-g','-gdwarf-2') + $DEFS + $INCS + @('-std=gnu11','-c')
$AFLAGS = @('-mcpu=cortex-m0plus','-mthumb','-mthumb-interwork','-Os','-ffunction-sections','-fdata-sections',
            '-Wall','-g','-gdwarf-2','-x','assembler-with-cpp',
            '-DUSE_HT32_CHIP=37','-DUSE_LIBCFG_RAM_SIZE=0x1000','-c')

$CSRC = @()
$CSRC += Get-ChildItem (Join-Path $PROJ 'app') -Filter *.c | ForEach-Object { $_.FullName }
$CSRC += Get-ChildItem (Join-Path $PROJ 'driver') -Filter *.c | ForEach-Object { $_.FullName }
$CSRC += Get-ChildItem (Join-Path $PROJ 'bsp') -Filter *.c | ForEach-Object { $_.FullName }
$CSRC += @(
  (Join-Path $PROJ 'system_ht32l5xxxx_01.c'),
  (Join-Path $PROJ 'HT32-IDE/ht32_op.c')
)
foreach ($f in @('ht32_cm0plus_misc','ht32l5xxxx_adc','ht32l5xxxx_bftm','ht32l5xxxx_ckcu','ht32l5xxxx_crc',
                 'ht32l5xxxx_exti','ht32l5xxxx_flash','ht32l5xxxx_gpio','ht32l5xxxx_i2c','ht32l5xxxx_mctm',
                 'ht32l5xxxx_pdma','ht32l5xxxx_pwrcu','ht32l5xxxx_rstcu','ht32l5xxxx_rtc','ht32l5xxxx_spi',
                 'ht32l5xxxx_tm','ht32l5xxxx_usart','ht32l5xxxx_wdt')) {
  $CSRC += (Join-Path $LIB "HT32L5xxxx_Driver/src/$f.c")
}
$CSRC += @(
  (Join-Path $UTIL 'ht32_board.c'),
  (Join-Path $UTIL 'common/i2c_eeprom.c'),
  (Join-Path $UTIL 'common/spi_flash.c'),
  (Join-Path $LIB 'HT32L5xxxx_Driver/src/ht32_retarget.c'),
  (Join-Path $LIB 'HT32L5xxxx_Driver/src/ht32_serial.c'),
  (Join-Path $LIB 'HT32L5xxxx_Driver/src/printf.c'),
  (Join-Path $LIB 'HT32L5xxxx_Driver/src/syscalls.c')
)
$ASRC = @(Join-Path $PROJ 'HT32-IDE/startup_ht32l5xxxx_gcc_01.S')

$objs = @(); $fail = @(); $warned = @()
foreach ($src in $CSRC) {
  if (-not (Test-Path $src)) { Write-Host "MISSING $src"; $fail += $src; continue }
  $obj = Join-Path $OBJD ((Split-Path $src -Leaf) -replace '\.c$', '.o')
  $res = & $GCC @CFLAGS -o $obj $src 2>&1
  $code = $LASTEXITCODE
  if ($res) { Set-Content -Path ([IO.Path]::ChangeExtension($obj,'log')) -Value $res -Encoding UTF8 }
  if ($code -ne 0) { Write-Host "FAIL $src (exit $code)"; $fail += $src }
  else { $objs += $obj; if ($res) { $warned += (Split-Path $src -Leaf) } }
}
foreach ($src in $ASRC) {
  $obj = Join-Path $OBJD 'startup_ht32l5xxxx_gcc_01.o'
  $res = & $GCC @AFLAGS -o $obj $src 2>&1
  $code = $LASTEXITCODE
  if ($res) { Set-Content -Path ([IO.Path]::ChangeExtension($obj,'log')) -Value $res -Encoding UTF8 }
  if ($code -ne 0) { Write-Host "FAIL $src (exit $code)"; $fail += $src } else { $objs += $obj }
}

Write-Host "compiled $($objs.Count) objects, failed $($fail.Count), with-warnings $($warned.Count)"
if ($warned.Count -gt 0) { Write-Host ("  warnings in: " + ($warned -join ', ')) }
if ($fail.Count -gt 0) { $fail | ForEach-Object { Write-Host "  FAILED: $_" }; return }

$LDFLAGS = @('-mcpu=cortex-m0plus','-mthumb','-mthumb-interwork','-Os','-ffunction-sections','-fdata-sections',
             '-Wall','-g','-gdwarf-2', "-T$PROJ/HT32-IDE/linker.ld",
             '-Xlinker','--gc-sections','-Xlinker','--no-warn-rwx-segments',
             "-Wl,-Map,$MAP", '--specs=nano.specs','--specs=nosys.specs','-lm',
             '-Wl,--gc-sections','-Wl,--print-memory-usage','-o',$ELF) + $objs
$ldres = & $GCC @LDFLAGS 2>&1
$ldres | Set-Content -Path (Join-Path $OUT 'link.log') -Encoding UTF8
Write-Host "link exit=$LASTEXITCODE"
$ldres | Out-String -Width 200
if ($LASTEXITCODE -eq 0) { & $SIZE $ELF }
