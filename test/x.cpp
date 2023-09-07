
#include<iostream>
using namespace std;

int test(int x){
    return (x==1)?1:(x + test(x-1));
}

int main(){
    cout<<test(8)<<endl;
    cout<<test(20)<<endl;
    //cout<<test(0)<<endl;
    cout<<test(2)<<endl;
    return 0;
}