//
// Created by 14685 on 2022/7/3.
//

#include "Gimbal.h"
#include <stdbool.h>
#include "controller.h"
#include "bsp_can.h"
#include "bsp_uart.h"
#include "Detect.h"
#include "ramp.h"

#include "keyboard.h"

//TODO:���ǽ�noaction�������ͷ���ǰ�Ķ�׼��������һ��

ImuTypeDef imu;    //����IMU��������ص�����
GimbalBackType gimbal_back_step;  //������һ����Ϊ`gimbal_back_step`��`GimbalBackType`���͵ı���
GimbalYawTypeDef gim;

/*��̨����ֵ*/
int32_t   pit_center_offset = 3616;
int32_t   yaw_center_offset = 4600;

/*���������������*/
First_Order_Filter_t mouse_y_lpf,mouse_x_lpf;

/* ��̨��ԽǶ�,unit: degree*/
float     pit_relative_angle;
volatile float  yaw_relative_angle;

/* �ϰ巢�͸��°������֮һ */
volatile float yaw_angle_ref_v;

/* gimbal pid parameter */
float yaw_angle_fdb = 0;
float pit_angle_fdb = 0;
float c[3] = {0};

/* ��̨��������Ƕ�(degree) */
float yaw_angle_ref;
float pit_angle_ref;

/* ��̨������� */
int16_t yaw_moto_current;
int16_t pit_moto_current;

bool_t recv_flag=false;   //���⴮�ڽ��ձ�־λ
/*�л��ֶ����Զ�ģʽ��ӦPID�����ı�־λ*/
static _Bool auto_pid_flag = 0;
static _Bool manual_pid_flag = 1;

//���25֡��ʷ��̬����
float angle_history[50];

/**
 * ��Gimbal_Get_information()��ȡgim.ctrl_mode
 * ��ΪGIMBAL_INIT����̨��ʼ��
 * ��ΪGIMBAL_CLOSE_LOOP_ZGYRO����̨����������ջ�����
 * ��ΪGIMBAL_AUTO����̨���Զ�ģʽ
 * ��ΪGIMBAL_RELAX����̨���ߴ����������������PID������Ϊ����ģʽ
 * **/

void gimbal_task(void const * argument){
    /*��ʼ����̨���Ʋ���*/
    Gimbal_Init_param();
    /*��ȡPreviousWakeTime����ȡ��ǰ����ʱ�䣨osKernelSysTick����ϵͳ����������ʱ�䣬���߳�������ͬ*/
    uint32_t Gimbal_Wake_time = osKernelSysTick();

    while (1){
        /*��ȡ��̨��������������Ϣ������Gimbal_Get_mode���õ�gim.ctrl_mode��ͨ������ģʽ��򣬵õ�gim.ctrl_modeΪGIMBAL_RELAX��������̨��ģʽ*/
        Gimbal_Get_information();

        switch (gim.ctrl_mode){

            case (GIMBAL_INIT):{
                Gimbal_Init_handle();
            }break;

            case (GIMBAL_CLOSE_LOOP_ZGYRO):{
                Gimbal_Loop_handle();
            }break;

            case (GIMBAL_AUTO):{
                Gimbal_Auto_control();
            }break;

            case (GIMBAL_RELAX):{
                Gimbal_Relax_handle();
            }break;
        }
        if(gim.ctrl_mode!=GIMBAL_RELAX){
            Gimbal_Control_moto();
        }



        /*������ʱ����֤Gimbal_TASK�̶���������*/
        osDelayUntil(&Gimbal_Wake_time, GIMBAL_PERIOD);
    }
}


void Gimbal_Get_information(void){
    /*��ȡIMU����*/
    IMU_Get_data(&imu);

    /*��ȡ��̨��ԽǶ�*/
    yaw_relative_angle = Gimbal_Get_relative_pos(YawMotor.RawAngle, yaw_center_offset)/22.75f;
    pit_relative_angle = Gimbal_Get_relative_pos(PitMotor.RawAngle, pit_center_offset)/22.75f;

    /*����PC�˼������*/
    PC_Handle_kb();

    /*��ȡ��̨��ǰģʽ������ֻ���ж���RELAX����INITģʽ*/
    Gimbal_Get_mode();
}


void Gimbal_Get_mode(void){
    /* gim.ac_mode = Remote_Is_action();
     gim.last_mode = gim.ctrl_mode;*/
    if(   glb_err.err_list[REMOTE_CTRL_OFFLINE].err_exist //ң��������
          || glb_err.err_list[GIMBAL_YAW_OFFLINE].err_exist  //yaw��������
          || glb_err.err_list[GIMBAL_PIT_OFFLINE].err_exist//pitch��������
          || rc.sw2 ==RC_DN){/*�����ģ�����߻��Ҳದ��ֵ��DNʱ������̨ΪGIMBAL_RELAXģʽ*/
        gim.ctrl_mode = GIMBAL_RELAX;
    }

    else if(gim.ctrl_mode == GIMBAL_RELAX){
        gim.ctrl_mode = GIMBAL_INIT;
    }

    gim.last_mode = gim.ctrl_mode;
}

/**
 * ���ڻ�ȡ��ȡ��̨��ԽǶȣ�����tmp������ƫ���������λ��**/

int16_t Gimbal_Get_relative_pos(int16_t raw_ecd, int16_t center_offset){
    int16_t tmp = 0;
    if (center_offset >= 4096){
        if (raw_ecd > center_offset - 4096)
            tmp = raw_ecd - center_offset;
        else
            tmp = raw_ecd + 8192 - center_offset;
    }
    else{
        if (raw_ecd > center_offset + 4096)
            tmp = raw_ecd - 8192 - center_offset;
        else
            tmp = raw_ecd - center_offset;
    }
    return tmp;
}


/*��̨��ʼ��������*/
void Gimbal_Init_handle(void){
    pit_angle_fdb = pit_relative_angle;
    yaw_angle_fdb = yaw_relative_angle;

    /* gimbal pitch back center */
    pit_angle_ref = pit_relative_angle * (1 - ramp_calc(&pit_ramp));

    switch (gimbal_back_step){
        //��pitch��û�л������֮ǰ�������yaw��Ļ���
        case PIT_BACK_STEP:{
            /* keep yaw unmove this time */
            yaw_angle_ref = gim.ecd_offset_angle;

            if(fabs(pit_angle_fdb) <= 2.0f)
                gimbal_back_step = YAW_BACK_STEP;
        }break;

        case YAW_BACK_STEP:{
            /* yaw back center after pitch arrive */
            yaw_angle_ref = yaw_relative_angle * ( 1 - ramp_calc(&yaw_ramp));

            if (fabs(yaw_angle_fdb) <= 0.5f)
                gimbal_back_step = BACK_IS_OK;
        }break;

        case BACK_IS_OK:{
            /* yaw arrive and switch gimbal state */
            if(rc.sw2 == RC_UP){
                gim.ctrl_mode = GIMBAL_CLOSE_LOOP_ZGYRO;
            }
            else if (rc.sw2 == RC_MI){
                gim.ctrl_mode = GIMBAL_AUTO;
            }

            gim.yaw_offset_angle = imu.angle_x;
            gim.pit_offset_angle = imu.angle_y;
            pit_angle_ref = 0;
            yaw_angle_ref = 0;
            /*��̨������ɣ����������������ֵ*/
            YawMotor.PID_Velocity.MaxOut=YAW_V_PID_MAXOUT_M;
            YawMotor.FFC_Velocity.MaxOut=YAW_V_FFC_MAXOUT;
            PitMotor.PID_Velocity.MaxOut=PITCH_V_PID_MAXOUT_M;

        }break;
    }
}

/*��̨����������գ�����ҡ�˻�����꣩�����ƴ�����*/
void Gimbal_Loop_handle(){
    /*��ͨģʽ��������ģʽ���໥�л�*/
    if(rc.sw2==RC_MI||rc.mouse.r==1){
        gim.ctrl_mode = GIMBAL_AUTO;
    }
        /*�л���ϣ�������ͨģʽ�Ŀ���*/
    else{
        if(manual_pid_flag == 0){
            PID_Reset_manual();
            manual_pid_flag = 1;
            auto_pid_flag = 0;
        }

        pit_angle_fdb = imu.angle_y - gim.pit_offset_angle;
        yaw_angle_fdb = imu.angle_x - gim.yaw_offset_angle;

        Gimbal_Control_yaw();
        Gimbal_Control_pitch();

        //����pitch��Ļ�Ƕ�
        if ((pit_relative_angle >= PIT_ANGLE_MIN) && (pit_relative_angle <= PIT_ANGLE_MAX)){
            VAL_LIMIT(pit_angle_ref, PIT_ANGLE_MIN, PIT_ANGLE_MAX);
        }
    }
}


void Gimbal_Control_yaw(void){
    float fx=First_Order_Filter_Calculate(&mouse_x_lpf,rc.mouse.x);
    //yaw��ĽǶ��ۼӣ���λdegree
    yaw_angle_ref += -rc.ch3 * RC_RATIO * GIMBAL_RC_MOVE_RATIO_YAW
                     -fx * KB_RATIO * GIMBAL_PC_MOVE_RATIO_YAW;
}

void Gimbal_Control_pitch(void){
    float fy=First_Order_Filter_Calculate(&mouse_y_lpf,rc.mouse.y);
    //pitch��ĽǶ��ۼӣ���λdegree
    pit_angle_ref += rc.ch4 * RC_RATIO * GIMBAL_RC_MOVE_RATIO_PIT
                     - fy * KB_RATIO * GIMBAL_PC_MOVE_RATIO_PIT;
}


void Gimbal_Auto_control(void){
    float gimbal_yaw = 0;
    float gimbal_pitch = 0;  //������λ�����͵���̨�Ƕ�
    float target_distance = 0; //��ʶ��Ŀ��ľ���
//    static bool_t com_protect = 1; //Ϊ1ʱһ֡���ݴ������

    /*����ģʽ������ͨģʽ���໥�л�*/
    if(rc.sw2==RC_UP){
        gim.ctrl_mode=GIMBAL_CLOSE_LOOP_ZGYRO;
    }
    /*�л���ϣ���������ģʽ�Ŀ���*/
    else{
        if(auto_pid_flag == 0){
            PID_Reset_auto();
            auto_pid_flag = 1;
            manual_pid_flag = 0;
        }

        //	float rate=0.3,a,b;
        static float last_p=0.0f,last_y=0.0f;
        float fx=0.0;

        fx=First_Order_Filter_Calculate(&mouse_x_lpf,rc.mouse.x);
        //yaw��ĽǶ��ۼӣ���λdegree
        static float manual_offset =0.0;
        manual_offset+=-rc.ch3 * RC_RATIO * GIMBAL_RC_MOVE_RATIO_YAW
                       -fx * KB_RATIO * GIMBAL_PC_MOVE_RATIO_YAW;

//	HAL_GPIO_TogglePin(GPIOG,GPIO_PIN_2);
//TODO:
        if(recv_flag) {  //ŷ����rpy��ʽ����
            if (!rpy_rx_data.DATA[0]){     //���ԽǶȿ���
                gimbal_yaw = *(int32_t*)&rpy_rx_data.DATA[1] / 1000;
                        /*(int32_t)(rpy_rx_data.DATA[4] << 24 | rpy_rx_data.DATA[3] << 16
                                       | rpy_rx_data.DATA[2] << 8 | rpy_rx_data.DATA[1])/1000;*/
                gimbal_pitch = *(int32_t*)&rpy_rx_data.DATA[5] / 1000;
                        /*(int32_t)(rpy_rx_data.DATA[8] << 24 | rpy_rx_data.DATA[7] << 16
                                         | rpy_rx_data.DATA[6] << 8 | rpy_rx_data.DATA[5])/1000;*/
            }
            else{     //��ԽǶȿ���
                gimbal_yaw = (*(int32_t*)&rpy_rx_data.DATA[1] / 1000) + pit_angle_fdb;
                        /*((int32_t)(rpy_rx_data.DATA[4] << 24 | rpy_rx_data.DATA[3] << 16
                                        | rpy_rx_data.DATA[2] << 8 | rpy_rx_data.DATA[1])/1000) + pit_angle_fdb;*/
                gimbal_pitch = (*(int32_t*)&rpy_rx_data.DATA[5] / 1000) + yaw_angle_fdb;
                        /*((int32_t)(rpy_rx_data.DATA[8] << 24 | rpy_rx_data.DATA[7] << 16
                                          | rpy_rx_data.DATA[6] << 8 | rpy_rx_data.DATA[5])/1000) + yaw_angle_fdb;*/
            }
            pit_angle_ref = gimbal_pitch * 0.7f + last_p * 0.3f;
            yaw_angle_ref = gimbal_yaw + manual_offset;
            last_p = gimbal_pitch;
            last_y = gimbal_yaw;
            target_distance = *(int32_t*)&rpy_rx_data.DATA[13] / 1000;  //��ȡĿ�����
            recv_flag = 0;
        }
        //ң����΢��
//    gimbal_yaw_control();
        //gimbal_pitch_control();
        //pit_angle_ref=pit_relative_angle+b;

        //����pit��Ļ�Ƕ�
        if ((pit_angle_ref >= PIT_ANGLE_MAX) && (pit_angle_ref <= PIT_ANGLE_MIN)){
            VAL_LIMIT(pit_angle_ref, PIT_ANGLE_MIN, PIT_ANGLE_MAX);
        }
        if ((yaw_angle_ref >= 170) && (yaw_angle_ref <= -170)){
            VAL_LIMIT(yaw_angle_ref, -170, 170);
        }

//    //����pitch����ԽǶȲ�
        pit_angle_fdb = pit_relative_angle;
//    //����yaw����ԽǶȲ�
//    yaw_angle_fdb = yaw_relative_angle;
        //����������ʱҲʹ��IMU
//    pit_angle_fdb = imu.angle_y-gim.pit_offset_angle;
        yaw_angle_fdb = imu.angle_x - gim.yaw_offset_angle;

        /*gimbal_pitch = 0;
        gimbal_yaw = 0;*/
    }

}

void Gimbal_Control_moto(void)
{
    yaw_moto_current = Motor_Angle_Calculate(&YawMotor, yaw_angle_fdb,imu.gyro_z,yaw_angle_ref);

    /* pitch�ḩ���Ƕ����� */
    float delta=pit_relative_angle+pit_angle_ref-pit_angle_fdb;
    if(delta-PIT_ANGLE_MAX>0)
        pit_angle_ref=pit_angle_fdb+PIT_ANGLE_MAX-pit_relative_angle;
    else if(delta-PIT_ANGLE_MIN<0)
        pit_angle_ref=pit_angle_fdb-(PIT_ANGLE_MIN-pit_relative_angle);

    VAL_LIMIT(pit_angle_ref, PIT_ANGLE_MIN, PIT_ANGLE_MAX);
//  /* pitch��Ԥ���ٶȼ��㣬��λdegree/s */
//  pit_speed_ref    = PID_Calc(&pid_pit, pit_angle_fdb, pit_angle_ref);    //degree
//  /* pitch������ѹ���� */

//	pit_moto_current = PID_Calc(&pid_pit_speed, imu.gyro_y, pit_speed_ref); //degree/s
//	//�˲�
//	pit_moto_current = 0.5*last_current + 0.5*pit_moto_current;
//	last_current = pit_moto_current;
//pitch���ٶ�Ϊgyro_x
    pit_moto_current = Motor_Angle_Calculate(&PitMotor, pit_angle_fdb,imu.gyro_x,pit_angle_ref);

    //���͵�������̨������
    GimbalMoto_Send_current(yaw_moto_current, pit_moto_current);
}


/*��̨���ߴ����������������PID������Ϊ����ģʽ*/
void Gimbal_Relax_handle(void){
    static uint8_t data[8];
    Gimbal_Back_param();

    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;
    data[4] = 0;
    data[5] = 0;
    data[6] = 0;
    data[7] = 0;
    Write_CAN(CONTROL_CAN, CAN_GIMBAL_ID, data);
}



void Gimbal_Back_param(void){
    gimbal_back_step = PIT_BACK_STEP;
    gim.ecd_offset_angle = yaw_relative_angle;
    YawMotor.PID_Velocity.MaxOut=YAW_V_PID_MAXOUT_M_INIT;
    YawMotor.FFC_Velocity.MaxOut=YAW_V_FFC_MAXOUT_INIT;
    PitMotor.PID_Velocity.MaxOut=PITCH_V_PID_MAXOUT_INIT_M;
    //TODO:���Ǽ���KIֵ
//    YawMotor.PID_Angle.Ki=80000;
    ramp_init(&pit_ramp, BACK_CENTER_TIME/GIMBAL_PERIOD);
    ramp_init(&yaw_ramp, BACK_CENTER_TIME/GIMBAL_PERIOD);
    //pid_pit_speed.max_output = 15000;
}


void Gimbal_Init_param(void){
    /* ��̨pitch����PID������ʼ�� */
    PID_Init(&PitMotor.PID_Velocity, PITCH_V_PID_MAXOUT_INIT_M, PITCH_V_PID_MAXINTEGRAL_M, 0,
             PITCH_V_PID_KP_M, PITCH_V_PID_KI_M, PITCH_V_PID_KD_M, 1000, 5000, PITCH_V_PID_LPF_M,
             PITCH_V_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral | OutputFilter | DerivativeFilter);
    c[0] = PITCH_V_FCC_C0_M;
    c[1] = PITCH_V_FCC_C1_M;
    c[2] = PITCH_V_FCC_C2_M;
    Feedforward_Init(&PitMotor.FFC_Velocity, PITCH_V_FFC_MAXOUT_M, c, PITCH_V_FCC_LPF_M, 4, 4);
//    LDOB_Init(&PitMotor.LDOB, 30000 * 0, 0.1, c, 0.00001, 4, 4);
    PID_Init(&PitMotor.PID_Angle, PITCH_A_PID_MAXOUT_M, PITCH_A_PID_MAXINTEGRAL_M, 0,
             PITCH_A_PID_KP_M, PITCH_A_PID_KI_M, PITCH_A_PID_KD_M, 5, 2, PITCH_A_PID_LPF_M, PITCH_A_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral | DerivativeFilter | Derivative_On_Measurement);
    c[0] = PITCH_A_FCC_C0_M;
    c[1] = PITCH_A_FCC_C1_M;
    c[2] = PITCH_A_FCC_C2_M;
    Feedforward_Init(&PitMotor.FFC_Angle, PITCH_A_FFC_MAXOUT_M, c, PITCH_A_FCC_LPF_M, 3, 3);
    PitMotor.Max_Out = PITCH_MOTOR_MAXOUT * 0.9f;

    /* ��̨yaw����PID������ʼ�� */
    PID_Init(&YawMotor.PID_Velocity, YAW_V_PID_MAXOUT_M_INIT, YAW_V_PID_MAXINTEGRAL_M, 0,
             YAW_V_PID_KP_M, YAW_V_PID_KI_M, YAW_V_PID_KD_M, 1000, 5000,
             YAW_V_PID_LPF_M, YAW_V_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral | DerivativeFilter);
    c[0] = YAW_V_FCC_C0;
    c[1] = YAW_V_FCC_C1;
    c[2] = YAW_V_FCC_C2;
    Feedforward_Init(&YawMotor.FFC_Velocity, YAW_V_FFC_MAXOUT_INIT, c, YAW_V_FCC_LPF, 4, 4);
//    LDOB_Init(&YawMotor.LDOB, 30000 * 0, 0.1, c, 0.00001, 4, 4);

    c[0] = YAW_A_FCC_C0;
    c[1] = YAW_A_FCC_C1;
    c[2] = YAW_A_FCC_C2;
    Feedforward_Init(&YawMotor.FFC_Angle, YAW_A_FFC_MAXOUT, c, YAW_A_FCC_LPF, 3, 3);
    PID_Init(&YawMotor.PID_Angle, YAW_A_PID_MAXOUT_M, YAW_A_PID_MAXINTEGRAL_M, 0.0,
             YAW_A_PID_KP_M, YAW_A_PID_KI_M, YAW_A_PID_KD_M, 5, 2, 0, 0, 0,
             Integral_Limit | Trapezoid_Intergral);
    YawMotor.Max_Out = YAW_MOTOR_MAXOUT * 0.9f;

    First_Order_Filter_Init(&mouse_x_lpf,0.014,0.1);
    First_Order_Filter_Init(&mouse_y_lpf,0.014,0.1);
    /* ����̨�ĳ�ʼ��״̬����Ϊ�ͷ� */
    gim.ctrl_mode = GIMBAL_RELAX;
}


void PID_Reset_manual(){
    PID_Init(&YawMotor.PID_Velocity, YAW_V_PID_MAXOUT_M, YAW_V_PID_MAXINTEGRAL_M, 0,
             YAW_V_PID_KP_M, YAW_V_PID_KI_M, YAW_V_PID_KD_M, 1000, 5000,
             YAW_V_PID_LPF_M, YAW_V_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&YawMotor.PID_Angle, YAW_A_PID_MAXOUT_M, YAW_A_PID_MAXINTEGRAL_M, 0.0,
             YAW_A_PID_KP_M, YAW_A_PID_KI_M, YAW_A_PID_KD_M, 5, 2, 0, 0, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&PitMotor.PID_Velocity, PITCH_V_PID_MAXOUT_M, PITCH_V_PID_MAXINTEGRAL_M, 0,
             PITCH_V_PID_KP_M, PITCH_V_PID_KI_M, PITCH_V_PID_KD_M, 1000, 5000,
             PITCH_V_PID_LPF_M, PITCH_V_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&PitMotor.PID_Angle, PITCH_A_PID_MAXOUT_M, PITCH_A_PID_MAXINTEGRAL_M, 0.0,
             PITCH_A_PID_KP_M, PITCH_A_PID_KI_M, PITCH_A_PID_KD_M, 5, 2, 0, 0, 0,
             Integral_Limit | Trapezoid_Intergral);
    c[0] = PITCH_V_FCC_C0_M,
            c[1] = PITCH_V_FCC_C1_M,
            c[2] = PITCH_V_FCC_C2_M,
            Feedforward_Init(&PitMotor.FFC_Velocity, PITCH_V_FFC_MAXOUT_M, c, PITCH_V_FCC_LPF_M, 4, 4);
//    LDOB_Init(&PitMotor.LDOB, 30000 * 0, 0.1, c, 0.00001, 4, 4);
    PID_Init(&PitMotor.PID_Angle, PITCH_A_PID_MAXOUT_M, PITCH_A_PID_MAXINTEGRAL_M, 0,
             PITCH_A_PID_KP_M, PITCH_A_PID_KI_M, PITCH_A_PID_KD_M, 5, 2, PITCH_A_PID_LPF_M, PITCH_A_PID_D_LPF_M, 0,
             Integral_Limit | Trapezoid_Intergral | DerivativeFilter | Derivative_On_Measurement);
    c[0] = PITCH_A_FCC_C0_M,
            c[1] = PITCH_A_FCC_C1_M,
            c[2] = PITCH_A_FCC_C2_M,
            Feedforward_Init(&PitMotor.FFC_Angle, PITCH_A_FFC_MAXOUT_M, c, PITCH_A_FCC_LPF_M, 3, 3);
}


void PID_Reset_auto(){
//	YawMotor.PID_Velocity.MaxOut=YAW_V_PID_MAXOUT_A;
//	YawMotor.PID_Velocity.IntegralLimit=YAW_V_PID_MAXINTEGRAL_A;
//	YawMotor.PID_Velocity.Kp=YAW_V_PID_KP_A;
//	YawMotor.PID_Velocity.Ki=YAW_V_PID_KI_A;
//	YawMotor.PID_Velocity.Kd=YAW_V_PID_KD_A;
//
//	YawMotor.PID_Angle.MaxOut=YAW_A_PID_MAXOUT_A;
//	YawMotor.PID_Angle.IntegralLimit=YAW_A_PID_MAXINTEGRAL_A;
//	YawMotor.PID_Angle.Kp=YAW_A_PID_KP_A;
//	YawMotor.PID_Angle.Ki=YAW_A_PID_KI_A;
//	YawMotor.PID_Angle.Kd=YAW_A_PID_KD_A;
    PID_Init(&YawMotor.PID_Velocity, YAW_V_PID_MAXOUT_A, YAW_V_PID_MAXINTEGRAL_A, 0,
             YAW_V_PID_KP_A, YAW_V_PID_KI_A, YAW_V_PID_KD_A, 1000, 5000,
             YAW_V_PID_LPF_A, YAW_V_PID_D_LPF_A, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&YawMotor.PID_Angle, YAW_A_PID_MAXOUT_A, YAW_A_PID_MAXINTEGRAL_A, 0.0,
             YAW_A_PID_KP_A, YAW_A_PID_KI_A, YAW_A_PID_KD_A, 5, 2, 0, 0, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&PitMotor.PID_Velocity, PITCH_V_PID_MAXOUT_A, PITCH_V_PID_MAXINTEGRAL_A, 0,
             PITCH_V_PID_KP_A, PITCH_V_PID_KI_A, PITCH_V_PID_KD_A, 1000, 5000,
             PITCH_V_PID_LPF_A, PITCH_V_PID_D_LPF_A, 0,
             Integral_Limit | Trapezoid_Intergral);
    PID_Init(&PitMotor.PID_Angle, PITCH_A_PID_MAXOUT_A, PITCH_A_PID_MAXINTEGRAL_A, 0.0,
             PITCH_A_PID_KP_A, PITCH_A_PID_KI_A, PITCH_A_PID_KD_A, 5, 2, 0, 0, 0,
             Integral_Limit | Trapezoid_Intergral);
    c[0] = PITCH_V_FCC_C0_A;
    c[1] = PITCH_V_FCC_C1_A;
    c[2] = PITCH_V_FCC_C2_A;
    Feedforward_Init(&PitMotor.FFC_Velocity, PITCH_V_FFC_MAXOUT_A, c, PITCH_V_FCC_LPF_A, 4, 4);
//    LDOB_Init(&PitMotor.LDOB, 30000 * 0, 0.1, c, 0.00001, 4, 4);
    PID_Init(&PitMotor.PID_Angle, PITCH_A_PID_MAXOUT_A, PITCH_A_PID_MAXINTEGRAL_A, 0,
             PITCH_A_PID_KP_A, PITCH_A_PID_KI_A, PITCH_A_PID_KD_A, 5, 2, PITCH_A_PID_LPF_A, PITCH_A_PID_D_LPF_A, 0,
             Integral_Limit | Trapezoid_Intergral | DerivativeFilter | Derivative_On_Measurement);
    c[0] = PITCH_A_FCC_C0_A;
    c[1] = PITCH_A_FCC_C1_A;
    c[2] = PITCH_A_FCC_C2_A;
    Feedforward_Init(&PitMotor.FFC_Angle, PITCH_A_FFC_MAXOUT_A, c, PITCH_A_FCC_LPF_A, 3, 3);
}

