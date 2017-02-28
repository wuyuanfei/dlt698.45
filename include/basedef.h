#ifndef BASEDEF_H_
#define BASEDEF_H_

#define	A_OUT						fprintf(stderr, "[%s][%s][%d]", __FILE__, __FUNCTION__, __LINE__);
#define	A_FPRINTF(fmt, arg...)		A_OUT fprintf(stderr, fmt, ##arg)



#endif//BASEDEF_H_