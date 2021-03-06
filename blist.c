#include"blist.h"
#include"stdio.h"
/* disclaimer */
/* my simple implementation of black list using an array, qsort and bsearch */
/* it hasn't size limitation */
/* if allocation memory error has been occured the program fails */
/* disson@yandex.ru */


blist blist_init(void)
{
  blist a;
  if((a=(blist)malloc(sizeof(_blist))) == NULL)
    return NULL;
  if((a->array=(blist_t*)calloc(STR_SIZE,sizeof(blist_t))) == NULL)
    return NULL;
  a->capacity=INIT_SIZE;
  a->q=0; // current number of elements
  return a;

}


int mycmp(const void* a,const void* b)
{
  blist_t x,y;
  x=*(blist_t*)a;
  y=*(blist_t*)b;
  return strcasecmp(x->value,y->value);
}

int blist_add_to(blist bl,const char* s)
{
  blist_t blt, *newptr;

  if(!(blt=(blist_t)malloc(sizeof(_blist_t))))
    return ALLOC_ERROR;
  if(!(blt->value=(char*)calloc(STR_SIZE,sizeof(char))))
    return ALLOC_ERROR;


  // search if element already is in the list

  strncpy(blt->value,s,STR_SIZE);

  if((bsearch(&blt,bl->array,bl->q,sizeof(blist_t),mycmp)))
    {
      free(blt);
      free(blt->value);
      return 0;// if not found
    }

  if(bl->q < bl->capacity)
    {
      *(bl->array+bl->q)=blt;
      bl->q++;
    }
  else
    {
      if(!(newptr=(blist_t*)realloc(bl->array,sizeof(blist_t)*(bl->capacity+INIT_SIZE))))
        return ALLOC_ERROR;
      else
        {
          bl->array=newptr;
          *(bl->array+bl->q)=blt;
          bl->q++;
          bl->capacity+=INIT_SIZE;
        }
    }
  //sort

  qsort(bl->array,bl->q,sizeof(blist_t),mycmp);

  return 0;


}

int blist_check(blist bl,const char* str)
{

  // dynamically creates an element
  blist_t b;
  int ret;
  if((b=(blist_t)malloc(sizeof(_blist_t))) == NULL)
    return ALLOC_ERROR;

  if((b->value=(char*)calloc(STR_SIZE,sizeof(char))) == NULL)
    return ALLOC_ERROR;
  strncpy(b->value,str,STR_SIZE);

  //binary search

  if((bsearch(&b,bl->array,bl->q,sizeof(blist_t),mycmp)) != NULL)
    ret=1;//found
  else
    ret=0; //not found
  free(b);
  free(b->value);
  return ret;
}


void blist_delete(blist bl)
{
  size_t i;
  for(i=0;i<bl->q;i++)
    free(bl->array[i]);

  free(bl);
}
