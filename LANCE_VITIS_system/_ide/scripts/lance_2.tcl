# Usage with Vitis IDE:
# In Vitis IDE create a Single Application Debug launch configuration,
# change the debug type to 'Attach to running target' and provide this 
# tcl script in 'Execute Script' option.
# Path of this script: C:\Users\fabio\Desktop\Desktop\GitHub\LANCE_VITIS_system\_ide\scripts\lance_2.tcl
# 
# 
# Usage with xsct:
# To debug using xsct, launch xsct and run below command
# source C:\Users\fabio\Desktop\Desktop\GitHub\LANCE_VITIS_system\_ide\scripts\lance_2.tcl
# 
connect -url tcp:127.0.0.1:3121
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Xilinx TUL 2222-tulA" && jtag_device_ctx=="jsn-TUL-2222-tulA-4ba00477-0"}
rst -system
after 3000
targets -set -filter {jtag_cable_name =~ "Xilinx TUL 2222-tulA" && level==0 && jtag_device_ctx=="jsn-TUL-2222-tulA-23727093-0"}
fpga -file C:/Users/fabio/Desktop/Desktop/GitHub/LANCE_VITIS/_ide/bitstream/LANCE_VIVADO.bit
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Xilinx TUL 2222-tulA" && jtag_device_ctx=="jsn-TUL-2222-tulA-4ba00477-0"}
loadhw -hw C:/Users/fabio/Desktop/Desktop/GitHub/LANCE_VIVADO/export/LANCE_VIVADO/hw/LANCE_VIVADO.xsa -mem-ranges [list {0x40000000 0xbfffffff}] -regs
configparams force-mem-access 1
targets -set -nocase -filter {name =~"APU*" && jtag_cable_name =~ "Xilinx TUL 2222-tulA" && jtag_device_ctx=="jsn-TUL-2222-tulA-4ba00477-0"}
source C:/Users/fabio/Desktop/Desktop/GitHub/LANCE_VITIS/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*A9*#0" && jtag_cable_name =~ "Xilinx TUL 2222-tulA" && jtag_device_ctx=="jsn-TUL-2222-tulA-4ba00477-0"}
dow C:/Users/fabio/Desktop/Desktop/GitHub/LANCE_VITIS/Debug/LANCE_VITIS.elf
configparams force-mem-access 0
targets -set -nocase -filter {name =~ "*A9*#0" && jtag_cable_name =~ "Xilinx TUL 2222-tulA" && jtag_device_ctx=="jsn-TUL-2222-tulA-4ba00477-0"}
con
