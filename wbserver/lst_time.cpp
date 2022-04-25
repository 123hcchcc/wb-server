#include "lst_time.h"



sort_timer_lst::~sort_timer_lst()
{
	util_timer* temp = head;
	while (temp)
	{
		head = head->next;
		delete temp;
		temp = head;
	}
}


void sort_timer_lst::add_timer(util_timer* timer)
{
	if (!timer)
	{
		return;
	}
	if (!head)
	{
		head = tail = timer;
		return;
	}
	//Ϊ������������ԣ����Ǹ���Ŀ�궨ʱ���ĳ���ʱ�������򣬣����ճ�ʱʱ���С�������������ʱʱ��С��head����Ϊhead���������˳�����
	if (timer->expire < head->expire)
	{
		timer->next = head;
		head->prev = timer;
		head = timer;
		return;
	}
	add_timer(timer, head);//��timer����head��,����һ�����غ�����ע�������
}


void sort_timer_lst::adjust_timer(util_timer* timer)
{
	if (!timer)
	{
		return;
	}
	util_timer* temp = timer->next;
	//���õ����������1.����β����2.���б仯������Ӱ����λ
	if (!temp || (timer->expire < temp->expire))
	{
		return;
	}
	//���Ϊͷ���ڵ㣬��õ�ʱ�临�Ӷ���ȡ�������²���
	if (timer == head)
	{
		head = head->next;
		head->prev = NULL;
		timer->next = NULL;
		add_timer(timer, head);
	}
	else//��Ϊͷ����Ҳ����ȡ�������룡
	{
		timer->prev->next = timer->next;
		timer->next->prev = timer->prev;
		add_timer(timer, timer->next);

	}
}

void sort_timer_lst::del_timer(util_timer* timer)
{
	if (!timer)
	{
		return;
	}
	//�����������������ʾ������ֻ��һ����ʱ������Ŀ�궨ʱ��
	if ((timer == head) && (timer == tail))
	{
		delete timer;
		head = NULL;
		tail = NULL;
		return;
	}
	//���������������������ʱ������Ŀ�궨ʱ���������ͷ��㣬�������ͷ�������ΪԴ�ڵ����һ���ڵ�
	if (timer == head)
	{
		head = head->next;
		head->prev = NULL;
		delete timer;
		return;
	}
	//���������������������ʱ������Ŀ��ڵ��������β�ڵ�
	if (timer == tail)
	{
		tail = tail->prev;
		tail->next = NULL;
		delete timer;
		return;
	}
	//�������������������ʱ��������Ŀ��ڵ�����������м�λ��
	else
	{
		timer->prev->next = timer->next;
		timer->next->prev = timer->prev;
		delete timer;
		return;
	}

}


void sort_timer_lst::tick()
{
	if (!head)
	{
		return;
	}
	time_t cur = time(NULL);//��ȡϵͳ��ǰʱ��
	util_timer* temp = head;

	//��ͷ�ڵ㿪ʼһ�δ���ÿһ����ʱ����ֱ������һ����δ���ڵĶ�ʱ��������Ƕ�ʱ���ĺ����߼�
	//˵���˾��Ǳ���

	while (temp)
	{
		//��Ϊÿ����ʱ�������þ���ʱ����Ϊ��ʱֵ���������ǿ��԰ɶ�ʱ���ĳ�ʱֵ��ϵͳ��ǰʱ�䣬�Ƚ�һ�ж϶�ʱ���Ƿ���
		if (cur < temp->expire)
		{
			break;
		}
		//���ö�ʱ���Ļص���������ִ�ж�ʱ����
		temp->cb_func(temp->user_data);
		//ִ���궨ʱ�����Ժ󣬾ͽ�����������ɾ����������������ͷ�ڵ�
		head = temp->next;
		//���������ʱ��һ��һ��Ҫע�⣡�ȼ���Ƿ�Ϊ��
		if (head)
		{
			head->prev = NULL;
		}
		delete temp;
		temp = head;
	}

}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head)
{
	util_timer* prev = lst_head;
	util_timer* temp = prev->next;
	//����head֮������нڵ㣬֪���ҵ�һ���ڵ��ֵ����timer��λ�ò���
	while (temp)
	{
		if (timer->expire < temp->expire)
		{
			prev->next = timer;
			timer->next = temp;
			temp->prev = timer;
			timer->prev = prev;
			break;
		}
		prev = temp;
		temp = temp->next;
	}
	//��������껹û���ҵ�����ֱ�Ӳ嵽β�ڵ�
	if (!temp)
	{
		prev->next = timer;
		timer->prev = prev;
		timer->next = NULL;
		tail = timer;
	}

}
