#include "bitstream_loader.h"


/***************************** Include Files *********************************/

#include "xparameters.h"
#include "xdevcfg.h"

/************************** Constant Definitions *****************************/
/*
 * The following constants map to the XPAR parameters created in the
 * xparameters.h file. They are only defined here such that a user can easily
 * change all the needed parameters in one place.
 */
#define DCFG_DEVICE_ID      XPAR_XDCFG_0_DEVICE_ID

/*
 * SLCR registers
 */
#define SLCR_LOCK	0xF8000004 /**< SLCR Write Protection Lock */
#define SLCR_UNLOCK	0xF8000008 /**< SLCR Write Protection Unlock */
#define SLCR_LVL_SHFTR_EN 0xF8000900 /**< SLCR Level Shifters Enable */
#define SLCR_PCAP_CLK_CTRL XPAR_PS7_SLCR_0_S_AXI_BASEADDR + 0x168 /**< SLCR PCAP clock control register address */
#define SLCR_PCAP_CLK_CTRL_EN_MASK 0x1
#define SLCR_LOCK_VAL	0x767B
#define SLCR_UNLOCK_VAL	0xDF0D



int load_partial_bitstream(int bit_stream_location,int bit_stream_size_words);



XDcfg DcfgInstance;   /* Device Configuration Interface Instance */


/*****************************************************************************/
/**
* This function downloads the Non secure bit stream to the FPGA fabric
* using the Device Configuration Interface.
*
* @param	bit_stream_location is the Bitstream location. ex 0x00400001
* @param	bit_stream_size_words is the size in words of the bitstream. ex 0xF6EC0.
*
* @return
*		- XST_SUCCESS if successful
*		- XST_FAILURE if unsuccessful
*
* @note		None
*
****************************************************************************/
int load_partial_bitstream(int bit_stream_location,int bit_stream_size_words)
{
	int Status;
	u32 IntrStsReg = 0;
	u32 StatusReg;
	u32 PartialCfg = 0;

	XDcfg_Config *ConfigPtr;

	/*
	 * Initialize the Device Configuration Interface driver.
	 */
	ConfigPtr = XDcfg_LookupConfig(DCFG_DEVICE_ID);

	/*
	 * This is where the virtual address would be used, this example
	 * uses physical address.
	 */
	Status = XDcfg_CfgInitialize(&DcfgInstance, ConfigPtr,
				     ConfigPtr->BaseAddr);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}


	Status = XDcfg_SelfTest(&DcfgInstance);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	/*
	 * Check first time configuration or partial reconfiguration
	 */
	IntrStsReg = XDcfg_IntrGetStatus(&DcfgInstance);
	if (IntrStsReg & XDCFG_IXR_DMA_DONE_MASK) {
		PartialCfg = 1;
	}

	/*
	 * Enable the pcap clock.
	 */
	StatusReg = Xil_In32(SLCR_PCAP_CLK_CTRL);
	if (!(StatusReg & SLCR_PCAP_CLK_CTRL_EN_MASK)) {
		Xil_Out32(SLCR_UNLOCK, SLCR_UNLOCK_VAL);
		Xil_Out32(SLCR_PCAP_CLK_CTRL,
			  (StatusReg | SLCR_PCAP_CLK_CTRL_EN_MASK));
		Xil_Out32(SLCR_UNLOCK, SLCR_LOCK_VAL);
	}

	/*
	 * Disable the level-shifters from PS to PL.
	 */
	if (!PartialCfg) {
		Xil_Out32(SLCR_UNLOCK, SLCR_UNLOCK_VAL);
		Xil_Out32(SLCR_LVL_SHFTR_EN, 0xA);
		Xil_Out32(SLCR_LOCK, SLCR_LOCK_VAL);
	}

	/*
	 * Select PCAP interface for partial reconfiguration
	 */
	if (PartialCfg) {
		XDcfg_EnablePCAP(&DcfgInstance);
		XDcfg_SetControlRegister(&DcfgInstance, XDCFG_CTRL_PCAP_PR_MASK);
	}

	/*
	 * Clear the interrupt status bits
	 */
	XDcfg_IntrClear(&DcfgInstance, (XDCFG_IXR_PCFG_DONE_MASK |
				      XDCFG_IXR_D_P_DONE_MASK |
				      XDCFG_IXR_DMA_DONE_MASK));

	/* Check if DMA command queue is full */
	StatusReg = XDcfg_ReadReg((&DcfgInstance)->Config.BaseAddr,
				  XDCFG_STATUS_OFFSET);
	if ((StatusReg & XDCFG_STATUS_DMA_CMD_Q_F_MASK) ==
	    XDCFG_STATUS_DMA_CMD_Q_F_MASK) {
		return XST_FAILURE;
	}

	/*
	 * Download bitstream in non secure mode
	 */
	XDcfg_Transfer(&DcfgInstance, (u8 *)bit_stream_location,
		       bit_stream_size_words,
		       (u8 *)XDCFG_DMA_INVALID_ADDRESS,
		       0, XDCFG_NON_SECURE_PCAP_WRITE);

	/* Poll IXR_DMA_DONE */
	IntrStsReg = XDcfg_IntrGetStatus(&DcfgInstance);
	while ((IntrStsReg & XDCFG_IXR_DMA_DONE_MASK) !=
	       XDCFG_IXR_DMA_DONE_MASK) {
		IntrStsReg = XDcfg_IntrGetStatus(&DcfgInstance);
	}

	if (PartialCfg) {
		/* Poll IXR_D_P_DONE */
		while ((IntrStsReg & XDCFG_IXR_D_P_DONE_MASK) !=
		       XDCFG_IXR_D_P_DONE_MASK) {
			IntrStsReg = XDcfg_IntrGetStatus(&DcfgInstance);
		}
	} else {
		/* Poll IXR_PCFG_DONE */
		while ((IntrStsReg & XDCFG_IXR_PCFG_DONE_MASK) !=
		       XDCFG_IXR_PCFG_DONE_MASK) {
			IntrStsReg = XDcfg_IntrGetStatus(&DcfgInstance);
		}
		/*
		 * Enable the level-shifters from PS to PL.
		 */
		Xil_Out32(SLCR_UNLOCK, SLCR_UNLOCK_VAL);
		Xil_Out32(SLCR_LVL_SHFTR_EN, 0xF);
		Xil_Out32(SLCR_LOCK, SLCR_LOCK_VAL);
	}

	return XST_SUCCESS;
}
